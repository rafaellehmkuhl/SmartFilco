#include "maquina.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
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

void mq_iniciar(void)
{
    hal_iniciar();
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
    ESP_LOGW(TAG, "falha reconhecida, voltando a ocioso");
}

void mq_shot_iniciar(void)
{
    if (estado == MQ_FALHA) return;
    // Deixar puxar shot com a máquina fria é como o painel mostrar número
    // inventado: tecnicamente funciona e o resultado é lixo.
    if (estado != MQ_PRONTA && estado != MQ_OCIOSO) return;
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

// ---------------------------------------------------------------- proteções
//
// Rodam ANTES do controle e podem derrubar tudo. Independentes da UI, como a
// docs/07 exige: nenhuma delas consulta estado de tela ou de rede.
// Devolvem CÓDIGO, não frase. A palavra que o operador lê é decisão da interface,
// que é quem sabe o idioma — mesmo princípio que já vale para as unidades: o
// firmware manda inteiro, a página escolhe como apresentar.
static void proteger(const hal_ent_t *e)
{
    if (e->falha_temp) { falha = "temp_aberta"; }
    else if (t_boiler_cru > T_MAX) { falha = "temp_alta"; }
    else if (extraindo() && e->falha_press) { falha = "press_faixa"; }
    else if (extraindo() && pressao > P_MAX) { falha = "press_alta"; }
    else if (extraindo() && shot_s > SHOT_TETO_S) { falha = "bomba_timeout"; }
    else if (extraindo() && nivel_mm > NIVEL_SECO_MM) { falha = "sem_agua"; }

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

    proteger(&e);

    if (estado == MQ_FALHA) {
        saida.ssr = 0.0f;
        saida.bomba = 0.0f;
        hal_escrever(&saida);
        hal_passo(dt_s);
        return;
    }

    // ---- temperatura: roda sempre, inclusive durante o shot
    float u = pid_passo(&pid_ctrl_temp, perfil.setpoint_c, t_boiler, dt_s, 0.0f, 1.0f);
    saida.ssr = fminf(fmaxf(u, 0.0f), 1.0f);

    bool na_faixa = fabsf(t_boiler - perfil.setpoint_c) <= PRONTA_JANELA;

    // ---- máquina de estados
    if (extraindo()) shot_s += dt_s;

    switch (estado) {
        case MQ_OCIOSO:
        case MQ_AQUECENDO:
            estado = na_faixa ? MQ_PRONTA : MQ_AQUECENDO;
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
            if (fim_s > 5.0f) { fim_s = 0.0f; estado = MQ_OCIOSO; }
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
}
