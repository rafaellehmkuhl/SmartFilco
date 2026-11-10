#include "maquina.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "autotune.h"
#include "hal.h"
#include "sd.h"

static const char *TAG = "maquina";

// Limites de segurança. Estes números NÃO são preferência de sabor — são o que a
// docs/07 define como proteção independente da UI. Mexer aqui é decisão de
// segurança, não de perfil.
#define T_MAX 160.0f          // acima disso a resistência corta, sem discussão
#define P_MAX 13.0f           // teto do sensor é 13,8 bar
#define SHOT_TETO_S 60.0f     // bomba nunca fica ligada indefinidamente
#define NIVEL_SECO_MM 240.0f  // perto de vazio: não deixar bombear a seco
#define PRONTA_JANELA 1.5f    // ±°C do setpoint para considerar pronta

#define NIVEL_CHEIO 40.0f
#define NIVEL_VAZIO 250.0f
#define DECLINIO_S 6.0f       // duração da rampa de alívio da bomba

// ---------------------------------------------------------------- PID

typedef struct {
    float kp, ki, kd;
    float i;            // termo integral acumulado
    float m_ant;        // MEDIDA anterior, não erro anterior — ver abaixo
    float d_filt;       // derivada passada por filtro
    float tau_d;        // constante do filtro da derivada, em segundos
    float i_max;
    bool  primeiro;
} pid_ctrl_t;

// ⚠️ **Derivada sobre a MEDIDA e FILTRADA.** A forma ingênua — `(e - e_ant)/dt`
// — amplifica ruído por `1/dt`, que a 100 Hz é um ganho de 100×. Medido na
// simulação: com ±0,08 °C de ruído de sensor, `de/dt` dava 8 K/s, e vezes
// kd = 0,6 isso enterrava o termo proporcional. O SSR chaveava 0/100% a esmo,
// o duty médio ficava ~50% independente do erro, e a caldeira subia até bater
// na proteção de 160 °C com setpoint em 93.
//
// Duas mudanças curam isso:
//  · derivar a MEDIDA em vez do erro — some o chute quando alguém muda o
//    setpoint, que senão vira um pico de saída sem nada ter acontecido na planta;
//  · passar a derivada por um filtro de 1ª ordem, o que atenua o ruído sem
//    atrasar a resposta ao que é sinal de verdade.
//
// Foi a simulação que expôs isto, e só porque ela tem ruído de sensor. Modelo
// sem ruído teria deixado passar até o hardware chegar — que é exatamente o
// tipo de coisa que a bancada existe para pegar antes.
static float pid_passo(pid_ctrl_t *p, float alvo, float medido, float dt,
                       float lo, float hi)
{
    float e = alvo - medido;

    if (p->primeiro) {
        p->m_ant = medido;
        p->d_filt = 0.0f;
        p->primeiro = false;
    }
    float dm = (medido - p->m_ant) / dt;
    p->m_ant = medido;
    float a = dt / (dt + p->tau_d);
    p->d_filt += (dm - p->d_filt) * a;

    // sinal negativo: medida SUBINDO deve reduzir a saída
    float termo_d = -p->kd * p->d_filt;

    float u = p->kp * e + p->ki * p->i + termo_d;
    // Anti-windup por integração condicional: não integrar quando a saída já
    // está saturada empurrando para o mesmo lado do erro. Aquecendo a 100% de
    // duty, mais integral não aquece mais rápido — só cria dívida para pagar
    // depois em overshoot.
    bool satura_alto = (u >= hi && e > 0.0f);
    bool satura_baixo = (u <= lo && e < 0.0f);
    if (!satura_alto && !satura_baixo) {
        p->i += e * dt;
        if (p->i > p->i_max) p->i = p->i_max;
        if (p->i < -p->i_max) p->i = -p->i_max;
        u = p->kp * e + p->ki * p->i + termo_d;
    }
    return u;
}

static void pid_zerar(pid_ctrl_t *p)
{
    p->i = 0.0f;
    p->d_filt = 0.0f;
    p->primeiro = true;
}

// ------------------------------------------------------- filtro de temperatura
//
// MEDIANA, não média — e a escolha é oposta à da pressão de propósito, porque o
// ruído é de outra natureza.
//
// A pressão sofre ONDULAÇÃO periódica (60 Hz da bomba), que a média anula quando
// a janela cobre um número inteiro de ciclos — está em `ads.c`. A temperatura não
// tem periodicidade: o que a estraga é OUTLIER, uma leitura SPI corrompida ou um
// pico isolado do conversor. Média espalha o intruso por toda a janela; mediana
// o ignora por completo.
//
// Foi a mesma lógica que resolveu o ultrassônico: uma amostra de 3 m no meio de
// 32 desloca a média em 9 cm e não move a mediana.
//
// 9 amostras a 100 Hz = 90 ms. O boiler tem ~400 J/K e sobe uns 2,5 °C/s no
// talo, então 90 ms são 0,2 °C de defasagem — invisível para o PID e barato
// perto de deixar um pico virar chute de derivada.
#define JAN_T 9
static float jt[JAN_T];
static uint8_t jt_n, jt_i;

static float mediana_temp(float x)
{
    jt[jt_i] = x;
    jt_i = (uint8_t)((jt_i + 1) % JAN_T);
    if (jt_n < JAN_T) jt_n++;

    float v[JAN_T];
    memcpy(v, jt, sizeof(float) * jt_n);
    // insertion sort: com 9 elementos é mais rápido que qsort e não aloca
    for (uint8_t i = 1; i < jt_n; i++) {
        float k = v[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
        v[j + 1] = k;
    }
    return v[jt_n / 2];
}

// ---------------------------------------------------------------- estado

// tau_d generoso na temperatura (2 s) porque planta térmica é lenta e o ruído do
// PT100 é rápido — não há informação útil acima de ~0,5 Hz. Na pressão o filtro é
// curto (0,05 s): ali existe dinâmica real e rápida, e atrasar demais a derivada
// tiraria justamente o amortecimento que ela deve dar contra a ondulação da bomba.
static pid_ctrl_t pid_ctrl_temp = {
    .kp = 0.08f, .ki = 0.004f, .kd = 0.6f,
    .tau_d = 2.0f, .i_max = 200.0f, .primeiro = true };
static pid_ctrl_t pid_press = {
    .kp = 0.09f, .ki = 0.05f, .kd = 0.004f,
    .tau_d = 0.05f, .i_max = 20.0f, .primeiro = true };

static mq_perfil_t perfil = {
    .setpoint_c = 93.0f,
    .pre_bar = 3.0f,
    .pre_s = 8.0f,
    .plato_bar = 9.0f,
    .rampa_s = 3.0f,
    .declinio_bar = 6.0f,
    .alvo_g = 36.0f,
    .tempo_max_s = 35.0f,
};

static mq_estado_t estado = MQ_OCIOSO;
static mq_estado_t estado_ant = MQ_OCIOSO;   // detecta a borda que fecha o arquivo
static const char *falha;
static float shot_s;
static float fim_s;          // quanto tempo o painel já mostrou o resultado
static float ult_g, ult_t;   // resumo do último shot, sobrevive à volta ao repouso
static const char *motivo = "";
static float t_log;          // relógio do log; 20 Hz basta para reconstruir o shot
static float massa_ini;      // tara no início do shot
static float duty_plato;     // duty da bomba quando o declínio começou
static float decl_s;         // tempo dentro do declínio
static float massa_ant, vazao_filt;
static float t_boiler, t_boiler_cru, pressao, massa, nivel_mm, alvo_p;
static hal_sai_t saida;
static void ganhos_carregar(void);
static void ignora_nivel_carregar(void);

void mq_iniciar(void)
{
    hal_iniciar();
    ganhos_carregar();
    ignora_nivel_carregar();
    estado = MQ_OCIOSO;
    falha = NULL;
}

void mq_perfil_get(mq_perfil_t *p) { *p = perfil; }
void mq_perfil_set(const mq_perfil_t *p) { perfil = *p; }

void mq_limpar_falha(void)
{
    if (estado != MQ_FALHA) return;
    falha = NULL;
    estado = MQ_OCIOSO;
    pid_zerar(&pid_ctrl_temp);
    // Repouso, não aquecendo: depois de uma falha quem decide religar é quem
    // olhou o que aconteceu.
    ESP_LOGW(TAG, "falha reconhecida, máquina em repouso");
}

static bool extraindo(void);

void mq_ligar(bool on)
{
    if (estado == MQ_FALHA) return;          // falha se resolve reconhecendo, não ligando
    if (on) {
        if (estado == MQ_OCIOSO) { pid_zerar(&pid_ctrl_temp); estado = MQ_AQUECENDO; }
    } else {
        // Desligar durante extração encerra o shot antes: bomba parando por
        // desligamento geral não pode deixar o shot pendurado em "extraindo".
        if (extraindo()) mq_shot_parar();
        estado = MQ_OCIOSO;
    }
    ESP_LOGI(TAG, "máquina %s", on ? "ligada" : "em repouso");
}

bool mq_ligada(void) { return estado != MQ_OCIOSO && estado != MQ_FALHA; }

void mq_shot_iniciar(bool forcar)
{
    if (estado == MQ_FALHA) return;
    // Deixar puxar shot com a máquina fria é como o painel mostrar número
    // inventado: tecnicamente funciona e o resultado é lixo.
    // Só de PRONTA. Antes aceitava de OCIOSO para facilitar teste, e isso deixava
    // puxar shot com a máquina fria — café ruim de forma silenciosa.
    if (estado != MQ_PRONTA && !(forcar && estado == MQ_AQUECENDO)) return;
    massa_ini = massa;         // tara: o que já está na balança não conta
    shot_s = 0.0f;
    t_log = 0.0f;
    pid_zerar(&pid_press);
    estado = MQ_PRE_INFUSAO;
    motivo = "";
    sd_shot_abrir();           // sem cartão simplesmente não grava; não impede o shot
    ESP_LOGI(TAG, "shot iniciado (alvo %.0f g)", perfil.alvo_g);
}

void mq_shot_parar(void)
{
    if (estado >= MQ_PRE_INFUSAO && estado <= MQ_DECLINIO) {
        motivo = "manual";
        ult_g = massa - massa_ini; ult_t = shot_s;
        fim_s = 0.0f; estado = MQ_FIM;
        ESP_LOGI(TAG, "shot parado em %.1f s / %.1f g", shot_s, massa - massa_ini);
    }
}

static bool extraindo(void)
{
    return estado >= MQ_PRE_INFUSAO && estado <= MQ_DECLINIO;
}

// ------------------------------------------- teste de bomba ou SSR (bancada)
//
// Um teste por vez, num atuador só: `teste_ssr` diz qual. O SSR tem regras
// próprias porque esquenta uma caldeira, não empurra água: só com a máquina
// DESLIGADA (senão o PID e o teste disputam a resistência, e o integral acumula
// dívida enquanto o teste manda), teto maior (a caldeira leva minutos para
// mostrar alguma coisa) e corte ao chegar no setpoint — o teste nunca aquece
// além do que a máquina ligada aqueceria.
//
// Prazo em RELÓGIO ABSOLUTO, não somando dt: o laço limita dt a 0,2 s por passo,
// então um engasgo de 2 s contaria só 0,2 — e o teste duraria mais do que o
// teto prometido. Com o prazo absoluto, o teto vale mesmo com a task atrasada.
//
// ponytail: escrito pelo handler de WS (core 0) e lido pelo laço (core 1) sem
// lock. O par é publicado com o prazo POR ÚLTIMO, então o laço nunca vê prazo
// válido com duty velho. int64 rasgado no pior caso dá um prazo absurdo por um
// passo — e o teto abaixo corta pelo relógio do PRÓPRIO laço também.
static volatile float teste_duty;
static volatile bool teste_ssr;
static volatile int64_t teste_ate_us;   // 0 = sem teste
static int64_t teste_ini_us;
static const char *teste_fim = "";

static bool cal_on;

void mq_teste(bool ssr, float duty, float s)
{
    if (estado == MQ_FALHA || extraindo() || cal_on) return;
    if (ssr && estado != MQ_OCIOSO) return;
    if (!(duty > 0.0f)) duty = 0.0f;     // pega NaN
    if (duty > 1.0f) duty = 1.0f;
    if (!(s > 0.0f)) return;
    float teto = ssr ? MQ_TESTE_SSR_MAX_S : MQ_TESTE_MAX_S;
    if (s > teto) s = teto;
    teste_ate_us = 0;        // para o anterior antes de trocar de atuador
    teste_ssr = ssr;
    teste_duty = duty;
    teste_fim = "";
    int64_t agora = esp_timer_get_time();
    teste_ini_us = agora;
    teste_ate_us = agora + (int64_t)(s * 1e6f);
    ESP_LOGW(TAG, "teste de %s: %.0f%% por %.1f s", ssr ? "ssr" : "bomba", duty * 100.0f, s);
}

void mq_teste_parar(void)
{
    if (teste_ate_us) teste_fim = "manual";
    teste_ate_us = 0;
}

// --------------------------------------------------------- perfil de pressão
//
// A pré-infusão é um **teto**, não um alvo: a bomba empurra até 3 bar e a
// pressão real fica onde o puck deixar. Tratá-la como alvo faria o PID forçar
// pressão contra um bolo ainda seco, que é o oposto da ideia.
// A docs/05 registra essa distinção; aqui ela vira código.
static float alvo_do_perfil(void)
{
    switch (estado) {
        case MQ_PRE_INFUSAO: return perfil.pre_bar;
        case MQ_RAMPA: {
            float k = shot_s > perfil.pre_s
                          ? (shot_s - perfil.pre_s) / fmaxf(perfil.rampa_s, 0.1f)
                          : 0.0f;
            if (k > 1.0f) k = 1.0f;
            return perfil.pre_bar + (perfil.plato_bar - perfil.pre_bar) * k;
        }
        case MQ_PLATO: return perfil.plato_bar;
        // no declínio isto NÃO é perseguido — é só a referência que vai ao gráfico
        // e ao CSV, para dar para comparar o que se pediu com o que o puck deu
        case MQ_DECLINIO: return perfil.declinio_bar;
        default: return 0.0f;
    }
}

// ------------------------------------------------- ganhos e autoajuste
//
// `ganhos[]` é a fonte da verdade; os pid_ctrl_t recebem cópia em ganhos_aplicar.
// ponytail: escrito pelo WS (core 0) e lido pelo laço (core 1) sem lock. Um passo
// com kp novo e ki velho é um degrau de 10 ms numa caldeira que leva minutos.
static const mq_ganhos_t GANHOS_PADRAO[MQ_MALHAS] = {
    { .kp = 0.08f, .ki = 0.004f, .kd = 0.6f },     // do simulador, não desta caldeira
    { .kp = 0.09f, .ki = 0.05f,  .kd = 0.004f },
};
static mq_ganhos_t ganhos[MQ_MALHAS];
static pid_ctrl_t *const PIDS[MQ_MALHAS] = { &pid_ctrl_temp, &pid_press };

static void ganhos_aplicar(mq_malha_t m)
{
    pid_ctrl_t *p = PIDS[m];
    const mq_ganhos_t *g = &ganhos[m];
    // Reescala o integral para o TERMO integral (ki·i) não pular com o ki novo,
    // e o teto do integral sai do ki: o integral sozinho alcança 100%, não mais.
    if (g->ki > 0.0f) { p->i *= p->ki / g->ki; p->i_max = 1.0f / g->ki; }
    p->kp = g->kp; p->ki = g->ki; p->kd = g->kd;
}

static void ganhos_salvar(void)
{
    nvs_handle_t h;
    if (nvs_open("cal", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "pid", ganhos, sizeof(ganhos));
    nvs_commit(h);
    nvs_close(h);
}

static void ganhos_carregar(void)
{
    memcpy(ganhos, GANHOS_PADRAO, sizeof(ganhos));
    nvs_handle_t h;
    if (nvs_open("cal", NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(ganhos);
        // Tamanho diferente = struct mudou de versão. Melhor o padrão do que ler
        // campos deslocados como se fossem ganhos.
        if (nvs_get_blob(h, "pid", ganhos, &n) != ESP_OK || n != sizeof(ganhos))
            memcpy(ganhos, GANHOS_PADRAO, sizeof(ganhos));
        nvs_close(h);
    }
    for (int m = 0; m < MQ_MALHAS; m++) {
        ganhos_aplicar(m);
        ESP_LOGI(TAG, "pid %s: kp=%.4f ki=%.5f kd=%.4f (%s)", m ? "pressão" : "temp",
                 ganhos[m].kp, ganhos[m].ki, ganhos[m].kd,
                 ganhos[m].origem == MQ_GANHO_CALIBRADO ? "calibrado" :
                 ganhos[m].origem == MQ_GANHO_MANUAL ? "manual" : "padrão");
    }
}

static float lim_f(float v, float lo, float hi) { return !(v > lo) ? lo : v > hi ? hi : v; }

void mq_ganhos_get(mq_malha_t m, mq_ganhos_t *g) { if (m < MQ_MALHAS) *g = ganhos[m]; }

void mq_ganhos_set(mq_malha_t m, float kp, float ki, float kd)
{
    if (m >= MQ_MALHAS) return;
    // Tetos largos, só contra dedo escorregado e NaN — o kd da caldeira já passa
    // de 1 com ganhos sensatos.
    ganhos[m].kp = lim_f(kp, 0.0f, 10.0f);
    ganhos[m].ki = lim_f(ki, 0.0f, 10.0f);
    ganhos[m].kd = lim_f(kd, 0.0f, 100.0f);
    ganhos[m].origem = MQ_GANHO_MANUAL;
    ganhos_aplicar(m);
    ganhos_salvar();
}

void mq_ganhos_padrao(mq_malha_t m)
{
    if (m >= MQ_MALHAS) return;
    ganhos[m] = GANHOS_PADRAO[m];
    ganhos_aplicar(m);
    ganhos_salvar();
}

// Sensor do reservatório ainda não é confiável na bancada. Ligado, o nível deixa
// de barrar shot e calibração — a bomba pode rodar a seco, e quem liga assume.
// Fica na NVS: vale até alguém desligar, não só até o próximo reinício.
static bool ignora_nivel;

static void ignora_nivel_carregar(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("cal", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "ignnivel", &v); nvs_close(h); }
    ignora_nivel = v;
}

void mq_ignora_nivel(bool on)
{
    ignora_nivel = on;
    nvs_handle_t h;
    if (nvs_open("cal", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "ignnivel", on);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGW(TAG, "sensor do reservatório %s", on ? "IGNORADO" : "de volta");
}

bool mq_ignorando_nivel(void) { return ignora_nivel; }

// Autoajuste. Pedido pelo WS, executado pelo laço: quem inicializa o relé é quem
// tem a medida na mão, e o estado do relé fica todo de um core só.
// Caldeira real (09/11/2026): aquecendo a 100% e cortando NO setpoint, subiu mais
// ~19 °C depois do corte e passou de 110. O calor guardado na resistência e na
// água leva dezenas de segundos para chegar ao sensor. Daí: 100% só até 20 °C
// abaixo, e relé de 0/25% — a amplitude da oscilação acompanha a potência.
#define CAL_T_ALTO 0.25f
#define CAL_T_PREAQ 20.0f
#define CAL_T_HIST 0.2f
#define CAL_T_MAX_S (60 * 60)    // relé fraco = ciclos longos
#define CAL_T_FOLGA 10.0f        // passou disto acima do alvo, algo está errado
#define CAL_P_ALVO 6.0f          // longe do teto de 13 bar mesmo com o relé passando
#define CAL_P_ALTO 0.8f
#define CAL_P_HIST 0.5f          // acima da ondulação da bomba, que com 0,3 chaveava sozinha
#define CAL_P_MAX_S 40.0f
#define CAL_P_SUBIR_S 12.0f      // sem chegar no alvo até aqui: não há puck
// Período crítico abaixo disto não é a planta, é ruído cruzando a histerese.
// A caldeira real leva minutos por ciclo; o simulador, sem atraso térmico,
// dava 0,4 s e "ok". Na pressão, um puck real leva segundos para despressurizar;
// a ondulação da bomba chaveia o relé a cada 2–3 ciclos da rede (< 0,1 s). Com
// puck (10/11/2026) a pressão sobe 0→6 bar em 0,5 s e cai 2,5 bar em 50 ms ao
// cortar: Tu real ≈ 0,19 s, ±1,2 bar. O piso de 0,6 s rejeitava a planta real.
#define CAL_T_TU_MIN 10.0f
#define CAL_P_TU_MIN 0.12f

static volatile int cal_pedido;           // 0 nada, 1+m iniciar malha m, -1 parar
static int cal_malha;
static at_t cal_at;
static float cal_s;
static const char *cal_fim = "";
static float cal_ku, cal_tu;             // do último resultado, aceito ou não

void mq_cal_iniciar(mq_malha_t m) { if (m < MQ_MALHAS) cal_pedido = 1 + m; }
void mq_cal_parar(void) { cal_pedido = -1; }

static void cal_acabar(const char *fim)
{
    cal_on = false;
    cal_fim = fim;
    ESP_LOGW(TAG, "autoajuste de %s acabou: %s", cal_malha ? "pressão" : "temperatura", fim);
}

static void cal_passo(const hal_ent_t *e, float dt_s)
{
    int ped = cal_pedido;
    cal_pedido = 0;
    if (ped == -1 && cal_on) cal_acabar("manual");
    if (ped > 0 && !cal_on) {
        if (estado != MQ_OCIOSO) { cal_fim = "maq"; return; }
        // No simulado o resultado seria do modelo, e gravaria por cima dos ganhos
        // da máquina real — a NVS é uma só.
        if (hal_modo() == HAL_SIM) { cal_fim = "sim"; return; }
        teste_ate_us = 0;                    // um dono por atuador
        cal_malha = ped - 1;
        cal_s = 0.0f;
        cal_fim = "";
        cal_on = true;
        if (cal_malha == MQ_MALHA_TEMP) {
            at_iniciar(&cal_at, perfil.setpoint_c, CAL_T_ALTO, 0.0f, CAL_T_HIST, 1, 2, t_boiler);
            at_preaquecer(&cal_at, perfil.setpoint_c - CAL_T_PREAQ);
        } else
            at_iniciar(&cal_at, CAL_P_ALVO, CAL_P_ALTO, 0.0f, CAL_P_HIST, 1, 3, pressao);
        ESP_LOGW(TAG, "autoajuste de %s começou", cal_malha ? "pressão" : "temperatura");
    }
    if (!cal_on) return;

    cal_s += dt_s;
    if (hal_modo() == HAL_SIM) { cal_acabar("sim"); return; }
    bool temp = cal_malha == MQ_MALHA_TEMP;
    if (estado != MQ_OCIOSO) { cal_acabar("maq"); return; }
    if (temp) {
        if (t_boiler > cal_at.alvo + CAL_T_FOLGA) { cal_acabar("passou"); return; }
        if (cal_s > CAL_T_MAX_S) { cal_acabar("tempo"); return; }
    } else {
        if (e->falha_press) { cal_acabar("press_faixa"); return; }
        if (!ignora_nivel && nivel_mm > NIVEL_SECO_MM) { cal_acabar("sem_agua"); return; }
        if (cal_s > CAL_P_MAX_S) { cal_acabar("tempo"); return; }
        if (cal_at.subidas == 0 && cal_at.ligado && cal_s > CAL_P_SUBIR_S) {
            cal_acabar("sem_pressao"); return;
        }
    }

    float u = at_passo(&cal_at, temp ? t_boiler : pressao, dt_s);
    if (temp) saida.ssr = u; else saida.bomba = u;

    if (!at_pronto(&cal_at)) return;

    float ku, tu, kp, ki, kd;
    at_resultado(&cal_at, &ku, &tu);
    cal_ku = ku; cal_tu = tu;
    // Caldeira com PID (a derivada filtrada a 2 s segura o sobressinal); pressão
    // com PI — a ondulação de 60 Hz da bomba faria a derivada trabalhar contra.
    (temp ? at_ganhos_pid : at_ganhos_pi)(ku, tu, &kp, &ki, &kd);
    if (tu < (temp ? CAL_T_TU_MIN : CAL_P_TU_MIN) ||
        !(kp > 0.0f && kp < 10.0f && ki >= 0.0f && ki < 10.0f && kd >= 0.0f && kd < 100.0f)) {
        ESP_LOGW(TAG, "autoajuste rejeitado: Ku=%.4f Tu=%.3f s → kp=%.4f ki=%.5f kd=%.4f", ku, tu, kp, ki, kd);
        cal_acabar("invalido");
        return;
    }
    if (temp) saida.ssr = 0.0f; else saida.bomba = 0.0f;
    ganhos[cal_malha] = (mq_ganhos_t){ .kp = kp, .ki = ki, .kd = kd, .ku = ku, .tu = tu,
                                       .origem = MQ_GANHO_CALIBRADO };
    ganhos_aplicar(cal_malha);
    ganhos_salvar();
    ESP_LOGW(TAG, "autoajuste: Ku=%.4f Tu=%.2f s → kp=%.4f ki=%.5f kd=%.4f", ku, tu, kp, ki, kd);
    cal_acabar("ok");
}

// ---------------------------------------------------------------- proteções
//
// Rodam ANTES do controle e podem derrubar tudo. Independentes da UI, como a
// docs/07 exige: nenhuma delas consulta estado de tela ou de rede.
// Devolvem CÓDIGO, não frase. A palavra que o operador lê é decisão da interface,
// que é quem sabe o idioma — mesmo princípio que já vale para as unidades: o
// firmware manda inteiro, a página escolhe como apresentar.
// Uma leitura ruim isolada não é sonda aberta. Tirar a máquina da tomada gera um
// pico que o MAX31865 acusa como sub/sobretensão num ciclo só (visto em
// 08/11/2026, com o PT100 lendo certo antes e depois) — e uma falha falsa dessas
// desliga a resistência no meio do shot. Só vira falha depois de TEMP_FALHA_S
// seguidos. Nesse meio tempo o hal entrega a última leitura boa: são no máximo
// ~0,4 °C de caldeira subindo sem ser vista, e o termostato de vapor segue em série.
#define TEMP_FALHA_S 0.5f

static void proteger(const hal_ent_t *e, float dt_s)
{
    static float temp_ruim_s;
    temp_ruim_s = e->falha_temp ? temp_ruim_s + dt_s : 0.0f;

    if (temp_ruim_s >= TEMP_FALHA_S) { falha = "temp_aberta"; }
    else if (t_boiler_cru > T_MAX) { falha = "temp_alta"; }
    else if (extraindo() && e->falha_press) { falha = "press_faixa"; }
    else if (extraindo() && pressao > P_MAX) { falha = "press_alta"; }
    else if (extraindo() && shot_s > SHOT_TETO_S) { falha = "bomba_timeout"; }
    else if (extraindo() && !ignora_nivel && nivel_mm > NIVEL_SECO_MM) { falha = "sem_agua"; }

    if (falha) estado = MQ_FALHA;
}

// ---------------------------------------------------------------- passo

void mq_passo(float dt_s)
{
    hal_ent_t e;
    hal_ler(&e);

    // ⚠️ CRU para a proteção, FILTRADO para o controle — e a ordem importa.
    // Sobreaquecimento não pode esperar 90 ms de janela para aparecer, e um pico
    // isolado que dispare a proteção erra para o lado seguro. Já o PID tem que
    // ver sinal limpo, senão a derivada vira chute a cada leitura ruim.
    t_boiler_cru = e.t_boiler;
    t_boiler = mediana_temp(e.t_boiler);
    pressao = e.pressao;
    massa = e.massa;
    nivel_mm = e.nivel_mm;

    // Vazão é a derivada da massa, e derivada de sinal ruidoso é ruído puro.
    // Filtro de 1ª ordem com ~0,5 s de constante: rápido o suficiente para ver
    // canalização, lento o suficiente para o número ser legível.
    if (dt_s > 0.0f) {
        float v = (massa - massa_ant) / dt_s;
        vazao_filt += (v - vazao_filt) * fminf(dt_s / 0.5f, 1.0f);
        massa_ant = massa;
    }

    proteger(&e, dt_s);

    // Sobrepressão no teste vira FALHA de verdade, igual a no shot. Só com o sensor
    // válido: sem ele não há leitura para julgar, e o teste segue com o teto de
    // tempo e a válvula de alívio da máquina — o mesmo que a máquina de fábrica.
    if ((teste_ate_us || (cal_on && cal_malha == MQ_MALHA_PRESS)) &&
        !e.falha_press && pressao > P_MAX && estado != MQ_FALHA) {
        falha = "press_alta";
        estado = MQ_FALHA;
    }

    if (estado == MQ_FALHA) {
        if (teste_ate_us) { teste_ate_us = 0; teste_fim = falha; }
        if (cal_on) { cal_on = false; cal_fim = falha; }
        saida.ssr = 0.0f;
        saida.bomba = 0.0f;
        hal_escrever(&saida);
        hal_passo(dt_s);
        return;
    }

    // ---- temperatura
    //
    // ⚠️ Em REPOUSO a resistência fica DESLIGADA, e isto é decisão de segurança,
    // não de conveniência. A docs/07 exige "estado seguro no boot e em falha: SSR
    // e bomba desligados", e a versão anterior subia aquecendo sozinha: no boot a
    // máquina ia para AQUECENDO e mandava 100% no SSR sem ninguém ter pedido.
    //
    // Passava despercebido enquanto `real_escrever` era vazio e o SSR não existia
    // no hardware. **Desde 16/08/2026 o driver existe e o SSR está ligado**, então
    // isto deixou de ser precaução teórica: sem esta guarda seria uma resistência
    // energizada durante o boot, justamente o intervalo em que não há PID vigiando.
    //
    // Zerar o integral junto importa: parado, o erro contra o setpoint continua
    // grande, e sem isto o PID acumularia dívida em repouso e despejaria potência
    // no instante em que alguém ligasse.
    if (estado == MQ_OCIOSO) {
        saida.ssr = 0.0f;
        pid_zerar(&pid_ctrl_temp);
    } else {
        float u = pid_passo(&pid_ctrl_temp, perfil.setpoint_c, t_boiler, dt_s, 0.0f, 1.0f);
        saida.ssr = fminf(fmaxf(u, 0.0f), 1.0f);
    }

    bool na_faixa = fabsf(t_boiler - perfil.setpoint_c) <= PRONTA_JANELA;

    // ---- máquina de estados
    if (extraindo()) shot_s += dt_s;

    switch (estado) {
        case MQ_OCIOSO:
            // Repouso não é transitório: só se sai daqui por mq_ligar().
            saida.bomba = 0.0f;
            break;

        case MQ_AQUECENDO:
            if (na_faixa) estado = MQ_PRONTA;
            saida.bomba = 0.0f;
            break;

        case MQ_PRONTA:
            if (!na_faixa) estado = MQ_AQUECENDO;
            saida.bomba = 0.0f;
            break;

        case MQ_PRE_INFUSAO:
            // sai quando o tempo acaba OU quando o puck já aceita a pressão de
            // pré-infusão — o que vier primeiro
            if (shot_s >= perfil.pre_s || pressao >= perfil.pre_bar * 0.95f)
                estado = MQ_RAMPA;
            break;

        case MQ_RAMPA:
            if (shot_s >= perfil.pre_s + perfil.rampa_s) estado = MQ_PLATO;
            break;

        case MQ_PLATO:
            if (massa - massa_ini >= perfil.alvo_g * 0.8f) {
                duty_plato = saida.bomba;   // ponto de partida do alívio
                decl_s = 0.0f;
                estado = MQ_DECLINIO;
            }
            break;

        case MQ_DECLINIO:
            decl_s += dt_s;
            break;

        case MQ_FIM:
            // Sem isto a máquina ficava presa em FIM para sempre — nem voltava a
            // "pronta" nem aceitava outro shot. A janela existe para o painel
            // mostrar o resultado antes de voltar ao repouso; o resumo do último
            // shot continua publicado depois, então nada se perde.
            saida.bomba = 0.0f;
            fim_s += dt_s;
            // Volta a AQUECENDO, não a repouso: a máquina continua ligada, e o
            // ciclo seguinte promove para PRONTA assim que a temperatura voltar.
            if (fim_s > 5.0f) { fim_s = 0.0f; estado = MQ_AQUECENDO; }
            break;

        default:
            break;
    }

    // Parada. Peso é o critério; tempo é rede de segurança — se a balança falhar,
    // o shot não pode virar infinito ([docs/05]).
    if (extraindo()) {
        if (massa - massa_ini >= perfil.alvo_g) {
            ESP_LOGI(TAG, "alvo de %.0f g atingido em %.1f s", perfil.alvo_g, shot_s);
            motivo = "peso";
            ult_g = massa - massa_ini; ult_t = shot_s;
            fim_s = 0.0f; estado = MQ_FIM;
        } else if (shot_s >= perfil.tempo_max_s) {
            ESP_LOGW(TAG, "parou por tempo (%.0f s) com %.1f g", shot_s, massa - massa_ini);
            motivo = "tempo";
            ult_g = massa - massa_ini; ult_t = shot_s;
            fim_s = 0.0f; estado = MQ_FIM;
        }
    }

    // ---- pressão
    alvo_p = alvo_do_perfil();
    if (estado == MQ_DECLINIO) {
        // ⚠️ MALHA ABERTA de propósito, e isto é decisão de projeto, não atalho.
        //
        // A docs/03 mediu que nesta máquina a pressão SÓ CAI sangrando pelo puck:
        // não há solenoide de 3 vias para despejar. Degrau para baixo não acontece
        // com nenhuma eletrônica. A docs/05 conclui: "implementar como redução
        // programada da potência da bomba, registrando a pressão resultante — e
        // NÃO como setpoint decrescente com PID. Se for setpoint, o integrador vai
        // acumular erro contra um limite físico e a saída vai grudar no fundo."
        //
        // A primeira versão desta máquina de estados fazia exatamente o proibido, e
        // o log do teste mostrava o sintoma: pressão 1,5 bar ACIMA do alvo, caindo
        // devagar, com a bomba presa em ~37% em vez de aliviar.
        //
        // Aqui a bomba é comandada, não a pressão. O número em bar do perfil vira
        // uma RAZÃO de potência (declinio/platô) porque a bomba é aproximadamente
        // monotônica em duty — é estimativa de alimentação direta, e a pressão que
        // sair é o que o puck permitir. É ela que o CSV grava.
        float razao = perfil.plato_bar > 0.5f
                          ? perfil.declinio_bar / perfil.plato_bar : 0.5f;
        float destino = duty_plato * fminf(fmaxf(razao, 0.0f), 1.0f);
        float k = fminf(decl_s / DECLINIO_S, 1.0f);
        saida.bomba = duty_plato + (destino - duty_plato) * k;
        pid_zerar(&pid_press);   // ao voltar a fechar a malha, sem dívida acumulada
    } else if (extraindo()) {
        float up = pid_passo(&pid_press, alvo_p, pressao, dt_s, 0.0f, 1.0f);
        saida.bomba = fminf(fmaxf(up, 0.0f), 1.0f);
    } else {
        saida.bomba = 0.0f;
        pid_zerar(&pid_press);
    }

    // ---- teste de bancada: sobrepõe a saída de UM atuador, e só ela. Quem
    //      assumir o atuador de verdade vence o teste — shot pega a bomba, máquina
    //      ligada pega o SSR. Dois donos para o mesmo atuador não pode.
    if (teste_ate_us) {
        int64_t agora = esp_timer_get_time();
        float teto = teste_ssr ? MQ_TESTE_SSR_MAX_S : MQ_TESTE_MAX_S;
        const char *fim = NULL;
        if (teste_ssr && estado != MQ_OCIOSO) fim = "maq";
        else if (!teste_ssr && extraindo()) fim = "shot";
        else if (teste_ssr && t_boiler_cru >= perfil.setpoint_c) fim = "temp";
        else if (agora >= teste_ate_us || agora - teste_ini_us > (int64_t)(teto * 1e6f)) fim = "tempo";

        if (fim) { teste_ate_us = 0; teste_fim = fim; }
        else if (teste_ssr) saida.ssr = teste_duty;
        else saida.bomba = teste_duty;
    }

    cal_passo(&e, dt_s);

    // ---- log em cartão, a 20 Hz. Grava DEPOIS do controle para a linha refletir
    //      a saída que de fato foi aplicada neste passo, não a do passo anterior.
    if (extraindo()) {
        t_log += dt_s;
        if (t_log >= 0.05f) {
            t_log = 0.0f;
            sd_shot_linha(shot_s, t_boiler, pressao, alvo_p, massa - massa_ini,
                          vazao_filt, saida.ssr, saida.bomba);
        }
    } else if (estado_ant != estado && (estado == MQ_FIM || estado == MQ_FALHA)) {
        sd_shot_fechar(estado == MQ_FALHA ? falha : motivo);
    }
    estado_ant = estado;

    hal_escrever(&saida);
    hal_passo(dt_s);
}

void mq_publicar(mq_estado_pub_t *o)
{
    o->estado = estado;
    o->t_boiler = t_boiler;
    o->pressao = pressao;
    o->massa = massa - (extraindo() || estado == MQ_FIM ? massa_ini : 0.0f);
    o->vazao = extraindo() ? vazao_filt : 0.0f;
    float pct = 100.0f * (NIVEL_VAZIO - nivel_mm) / (NIVEL_VAZIO - NIVEL_CHEIO);
    o->nivel_pct = fminf(fmaxf(pct, 0.0f), 100.0f);
    o->alvo_pressao = alvo_p;
    o->ssr = saida.ssr;
    o->bomba = saida.bomba;
    o->shot_s = shot_s;
    o->falha = falha;
    o->pronta = (estado == MQ_PRONTA);
    o->motivo = motivo;
    o->ult_g = ult_g;
    o->ult_t = ult_t;
    int64_t resta = teste_ate_us ? teste_ate_us - esp_timer_get_time() : 0;
    o->teste_s = resta > 0 ? (float)resta / 1e6f : 0.0f;
    o->teste_fim = teste_fim;
    o->teste_ssr = teste_ssr;
    o->cal_on = cal_on;
    o->cal_malha = cal_malha;
    o->cal_passo = cal_on ? cal_at.subidas : 0;
    o->cal_total = cal_at.descartar + cal_at.medir + 1;
    o->cal_fim = cal_fim;
    o->cal_ku = cal_ku; o->cal_tu = cal_tu;
}
