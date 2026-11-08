// HAL real + o despacho entre real e simulado.
//
// **Estado dos periféricos (16/08/2026):** temperatura, pressão e nível lêem de
// verdade — MAX31865, ADS1115 e HC-SR04 — e os dois atuadores agora têm driver:
// SSR por janela lenta de PWM, bomba por PSM sobre a ISR de zero-cross. Falta só a
// célula de carga (amplificador não decidido).
//
// ⚠️ **O driver dos atuadores nunca rodou contra rede AC.** Compila e a conta do
// PSM está verificada em `ferramentas/teste_psm.py`, mas o número que decide se a
// sincronia está certa — `zc_hz`, que tem que dar ~120 — só existe com a máquina
// na tomada. Até alguém ler esse número, isto é código plausível, não código
// validado, e a distinção é exatamente a que esta bancada aprendeu a não apagar.
//
// Cada leitura ausente devolve `falha_* = true` em vez de zero — porque zero é um
// número plausível, e número plausível vindo de sensor inexistente é o modo de
// falha que mais custou nesta bancada (o ToF lendo 38 cm com status "válido").
//
// Trocar para hardware é preencher as funções marcadas com PENDENTE. A lógica de
// máquina não muda uma linha.

#include "hal.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ads.h"
#include "max.h"
#include "ultra.h"

static const char *TAG = "hal";

// implementadas em hal_sim.c
void sim_iniciar(void);
void sim_passo(float dt_s, const hal_sai_t *s);
void sim_ler(hal_ent_t *e);

// Nasce em REAL desde 11/08/2026. Enquanto não havia sensor nenhum ligado,
// simulado era o único modo que mostrava alguma coisa; agora temperatura, pressão
// e nível são leitura de verdade, e abrir a página em simulado passou a esconder
// justamente o que se quer ver.
//
// Consequência esperada, não defeito: com um sensor faltando a máquina cai em
// FALHA logo no boot, porque `proteger()` derruba com `falha_temp`. É o
// comportamento certo — e a página tem "falha limpar" para reconhecer, embora ela
// volte a cair enquanto o fio estiver solto.
static hal_modo_t s_modo = HAL_REAL;
static hal_sai_t s_saida;

// ---------------------------------------------------------------- real

static void real_ler(hal_ent_t *e)
{
    // ✅ MAX31865 ligado. A falha vem do registrador 0x07 do chip, não de palpite
    // sobre o valor: sonda aberta e boiler frio dão números parecidos e consertos
    // opostos. Sem leitura válida, congela o último bom — zero seria "boiler
    // gelado", que faz o controle mandar potência máxima na resistência.
    max_med_t t;
    static float t_bom = 20.0f;
    if (max_ler(&t) == ESP_OK && t.valida) {
        t_bom = t.celsius;
        e->falha_temp = false;
    } else {
        e->falha_temp = true;
    }
    e->t_boiler = t_bom;

    // ✅ ADS1115 ligado, em modo contínuo — ler não bloqueia o laço.
    ads_med_t a;
    if (ads_ler(&a) == ESP_OK) {
        e->pressao = a.bar;
        // Fora da faixa elétrica não é "pressão estranha", é FIO: sensor solto,
        // sem alimentação ou saída em curto. Reportar como falha em vez de como
        // pressão evita a máquina tentar controlar contra um número inventado.
        e->falha_press = !a.valida;
    } else {
        e->pressao = 0.0f;
        e->falha_press = true;
    }

    // PENDENTE — célula de carga. Amplificador ainda não decidido
    // ([docs/02]): NAU7802 no I²C ou HX711 em 2 GPIOs.
    e->massa = 0.0f;
    e->falha_massa = true;

    // ✅ HC-SR04 ligado. Dispara agora e lê o eco do disparo ANTERIOR — o eco
    // leva até 25 ms para voltar e este laço roda a 100 Hz, então esperar aqui
    // seria bloquear o controle por 2,5 ciclos.
    // A task do ultrassônico amostra a 20 Hz e entrega a MEDIANA de 1,6 s — ler
    // aqui é só pegar o resultado, sem bloquear o laço de controle.
    ultra_med_t u;
    ultra_ler(&u);
    // Sem janela válida, congela o último bom em vez de zerar. Zero = "tanque
    // cheio", e mandar a máquina achar que tem água por causa de uma janela
    // perdida é exatamente o contrário da proteção de bomba a seco.
    static float ult_bom = 250.0f;
    if (u.valida) ult_bom = u.mm;
    e->nivel_mm = ult_bom;
}

// Pinos dos atuadores. Ainda sem driver, mas JÁ COM ESTADO DEFINIDO.
#define PIN_ZC 4     // entrada: pulso de cruzamento por zero, vindo do dimmer
#define PIN_PSM 5    // saída: gate do TRIAC
#define PIN_SSR 6    // saída: resistência do boiler

// ⚠️ Chamada o mais cedo possível, ANTES de Wi-Fi, tasks e qualquer sensor.
//
// O comentário anterior deste arquivo afirmava que "estado seguro é o que já
// está: saída desenergizada". **Isso nunca foi verificado, e a premissa é falsa
// em geral.** Sem configuração, GPIO4/5/6 ficam em ENTRADA FLUTUANTE — e o que
// o dimmer faz com isso depende do circuito de entrada DELE, não do ESP32. Se a
// entrada `PSM` do módulo tiver pull-up (vários têm), flutuante é lido como
// nível alto: TRIAC disparando todo semiciclo, bomba no talo assim que a máquina
// for para a tomada.
//
// Aqui os dois viram SAÍDA EM NÍVEL BAIXO explícito, que é o desligado dos dois
// módulos (o opto só conduz com corrente).
//
// 🔴 O QUE ISTO **NÃO** COBRE: a janela entre o power-on e esta função rodar —
// bootloader mais init do IDF, algumas centenas de ms — em que os pinos seguem
// flutuando. Software não alcança esse intervalo. O conserto é HARDWARE:
// **resistor de pull-down de 10 k em cada linha**, no lado do dimmer e do SSR.
// Ele também cobre reset, travamento e o ESP32 fora do soquete, que são
// justamente os casos em que não há firmware para garantir nada.
void hal_atuadores_seguros(void)
{
    gpio_config_t saidas = {
        .pin_bit_mask = (1ULL << PIN_PSM) | (1ULL << PIN_SSR),
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,   // reforça o nível durante o deinit
    };
    ESP_ERROR_CHECK(gpio_config(&saidas));
    gpio_set_level(PIN_PSM, 0);
    gpio_set_level(PIN_SSR, 0);
    ESP_LOGW(TAG, "atuadores em nível baixo (GPIO%d e GPIO%d)", PIN_PSM, PIN_SSR);
}

// ------------------------------------------------------------ PSM (bomba)
//
// Bresenham sobre semiciclos: a cada cruzamento por zero, `acc += alvo`; quando
// transborda a escala, este semiciclo passa. O resto fica no acumulador, então
// os disparos saem DISTRIBUÍDOS em vez de agrupados — que é a diferença entre
// bomba pulsando parelho e bomba engasgando ([docs/02]).
//
// ⚠️ **Aritmética INTEIRA, e não é preferência de estilo.** No Xtensa os
// registradores da FPU não são salvos no contexto de interrupção. Um `float` aqui
// dentro corrompe a conta de quem foi interrompido — e o sintoma é um valor errado
// aparecendo em outra task, longe daqui, sem nada apontando para esta ISR.
#define PSM_ESC 1000

static volatile int32_t psm_alvo;     // 0..PSM_ESC, escrito pela task de controle
static volatile uint32_t zc_cont;

// Borda de ZC mais próxima da anterior que isto é ruído, não cruzamento. A 60 Hz
// o intervalo real é 8,33 ms; uma borda dupla (pulso largo, ringing do opto)
// contaria duas vezes, e com a decisão por ciclo abaixo isso desalinharia o par
// de semiciclos.
#define ZC_MIN_US 4000

// O gate fica alto pelo SEMICICLO INTEIRO, não em pulso curto. A bomba é um
// solenoide: a corrente atrasa em relação à tensão, e um pulso de gate estreito
// solta o TRIAC antes de a corrente firmar — ele comuta cedo, o curso do pistão
// não completa, e a bomba zumbe sem bombear. Mantendo o gate alto, o semiciclo
// sai inteiro. O preço é o opto conduzindo o tempo todo em duty alto, que é
// consumo previsto do módulo, não abuso.
//
// 🔴 **Decide por CICLO COMPLETO, não por semiciclo** — é o `divider = 2` da
// docs/02. As Ulka EP5/EX5 têm diodo em série: só uma polaridade produz curso.
// Decidindo por semiciclo, Bresenham a 50% sai "passa, pula, passa, pula" — e
// todo "passa" cai NA MESMA polaridade. Conforme a fase do boot, isso é 100% da
// vazão ou ZERO, com o painel mostrando 50% nos dois casos. Em 33% os disparos
// alternam de polaridade e saem pela metade. Mapa duty→vazão sem sentido nenhum.
// Por ciclo, cada "passa" leva as duas polaridades: funciona igual com diodo e
// sem diodo, e não depende de saber o modelo da bomba. Custa metade da
// resolução (60 decisões/s), que a bomba nem enxerga.
static void IRAM_ATTR zc_isr(void *arg)
{
    static int32_t acc;
    static int64_t t_ant;
    static bool segunda;   // segunda metade do ciclo: mantém a decisão da primeira

    int64_t t = esp_timer_get_time();
    if (t - t_ant < ZC_MIN_US) return;
    t_ant = t;
    zc_cont++;

    int32_t a = psm_alvo;
    if (a <= 0) { acc = 0; segunda = false; gpio_set_level(PIN_PSM, 0); return; }

    segunda = !segunda;
    if (!segunda) return;

    acc += a;
    if (acc >= PSM_ESC) { acc -= PSM_ESC; gpio_set_level(PIN_PSM, 1); }
    else                { gpio_set_level(PIN_PSM, 0); }
}

// ------------------------------------------------------------ SSR (boiler)
//
// Janela lenta de PWM. 1 s porque o SSR-40DA chaveia no zero da rede — ele só
// consegue ligar/desligar a cada 8,3 ms, então janela curta viraria quantização
// grosseira sem ganhar nada numa caldeira que leva minutos para responder.
#define SSR_JANELA_US 1000000

static int64_t ssr_t0;
static float ssr_travado;   // duty congelado no início da janela

// ------------------------------------------------- guarda de comando parado
//
// Sem isto, "task de controle travada" deixa o SSR no último nível — que pode ser
// 100%. O backstop de 135 °C ainda pega, mas 40 °C depois e sem ninguém saber por
// quê. Aqui a saída EXPIRA: se `real_escrever` parar de chegar, os dois pinos caem.
//
// Cobre de graça outros dois casos que já existem no projeto: o OTA, que suspende
// o laço de controle antes de gravar, e a troca para modo SIMULADO, em que
// `real_escrever` deixa de ser chamado — nos dois a máquina fica quieta sozinha.
#define GUARDA_US 250000

static volatile int64_t ult_cmd;
static esp_timer_handle_t s_guarda;

static void guarda_cb(void *arg)
{
    // 🔴 ZC sumiu com o gate alto = bomba LIGADA DIRETO. A ISR é quem baixa o
    // pino, e sem borda não há ISR: fio do Z-C solto no meio de um shot deixaria
    // o TRIAC conduzindo todo semiciclo até o fim do shot, sem controle. Em 100 ms
    // chegam ~12 bordas; nenhuma é perda de sincronismo, e o pino cai. Quando a
    // rede volta, a ISR retoma sozinha.
    static uint32_t zc_visto;
    if (zc_cont == zc_visto) gpio_set_level(PIN_PSM, 0);
    zc_visto = zc_cont;

    if (esp_timer_get_time() - ult_cmd > GUARDA_US) {
        psm_alvo = 0;
        ssr_travado = 0.0f;
        gpio_set_level(PIN_PSM, 0);
        gpio_set_level(PIN_SSR, 0);
    }
}

static float zc_hz;

static void real_escrever(const hal_sai_t *s)
{
    int64_t agora = esp_timer_get_time();
    ult_cmd = agora;

    // `!(x > 0)` e não `x < 0`: pega NaN também. NaN falha em toda comparação,
    // passaria pelos dois testes, e `(int32_t)NaN` é comportamento indefinido —
    // um duty qualquer, inclusive cheio.
    float bomba = !(s->bomba > 0.0f) ? 0.0f : (s->bomba > 1.0f ? 1.0f : s->bomba);
    float ssr   = !(s->ssr   > 0.0f) ? 0.0f : (s->ssr   > 1.0f ? 1.0f : s->ssr);

    // ---- bomba: só publica o alvo; quem comanda o pino é a ISR, no ritmo da rede
    psm_alvo = (int32_t)(bomba * PSM_ESC + 0.5f);
    // Sem rede não há ISR, e aí o pino ficaria congelado no último nível alto.
    // Zerar aqui garante que "duty 0" desliga mesmo com o dimmer sem alimentação.
    if (psm_alvo == 0) gpio_set_level(PIN_PSM, 0);

    // ---- resistência
    if (ssr_t0 == 0) ssr_t0 = agora;
    int64_t dec = agora - ssr_t0;
    if (dec >= SSR_JANELA_US) {
        // Ressincroniza se a task ficou parada muito tempo, em vez de correr atrás
        // de várias janelas perdidas.
        if (dec > 4 * SSR_JANELA_US) ssr_t0 = agora;
        else ssr_t0 += SSR_JANELA_US * (dec / SSR_JANELA_US);
        dec = agora - ssr_t0;
        ssr_travado = ssr;      // o duty entra em vigor no INÍCIO da janela
    }
    // ⚠️ Assimetria de propósito: LIGAR espera a próxima janela, DESLIGAR é
    // imediato. Falha e repouso mandam 0, e esses não podem ficar até 1 s
    // esperando a borda da janela para tirar potência da resistência.
    if (ssr <= 0.0f) ssr_travado = 0.0f;

    gpio_set_level(PIN_SSR, ((float)dec / (float)SSR_JANELA_US) < ssr_travado);

    // ---- saúde da detecção de zero
    static uint32_t zc_ant;
    static int64_t zc_t;
    if (zc_t == 0) zc_t = agora;
    if (agora - zc_t >= 1000000) {
        zc_hz = (float)(zc_cont - zc_ant) * 1000000.0f / (float)(agora - zc_t);
        zc_ant = zc_cont;
        zc_t = agora;
    }
}

// Um pino de zero-cross mudo é bomba que não bombeia com o painel jurando que
// está mandando duty — o tipo de falha silenciosa que esta bancada persegue.
// Publicado para a tela de debug julgar: em 60 Hz o esperado é ~120 Hz.
void hal_atuador_diag(hal_atuador_diag_t *o)
{
    o->zc_hz = zc_hz;
    o->zc_total = zc_cont;
    o->psm_duty = (float)psm_alvo / (float)PSM_ESC;
    o->ssr_duty = ssr_travado;
}

static void atuadores_iniciar(void)
{
    gpio_config_t zc = {
        .pin_bit_mask = 1ULL << PIN_ZC,
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&zc));

    // Mesmo serviço que o ultrassônico já instala, com a mesma flag: ISR fora de
    // IRAM PARA durante escrita em flash, e escrita em flash é o que o OTA faz.
    esp_err_t r = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "isr service: %s", esp_err_to_name(r));
        return;
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_ZC, zc_isr, NULL));

    const esp_timer_create_args_t g = { .callback = guarda_cb, .name = "guarda_at" };
    ESP_ERROR_CHECK(esp_timer_create(&g, &s_guarda));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_guarda, 100000));

    // 🔴 PRIMEIRO TESTE DE BANCADA: este número tem que dar ~120. O módulo pulsa
    // uma vez por cruzamento, e são dois por ciclo de 60 Hz. Se aparecer ~60, o
    // pino está dando um pulso por ciclo e o duty sai pela metade; se aparecer
    // ~240, a borda está sendo contada duas vezes e o duty sai dobrado. Nos dois
    // casos a bomba "quase funciona", que é o pior jeito de errar.
    ESP_LOGW(TAG, "atuadores vivos: ZC=GPIO%d (esperado ~120 Hz), PSM=GPIO%d, SSR=GPIO%d",
             PIN_ZC, PIN_PSM, PIN_SSR);
}

// ---------------------------------------------------------------- despacho

static i2c_master_bus_handle_t hal_bus;

void hal_set_bus(void *bus) { hal_bus = (i2c_master_bus_handle_t)bus; }

void hal_iniciar(void)
{
    sim_iniciar();
    // Sobe mesmo em modo simulado: a ISR de zero-cross só CONTA se não houver
    // duty, e ter a contagem viva permite conferir a rede sem colocar a máquina
    // em modo real — mesma razão dos sensores logo abaixo.
    atuadores_iniciar();
    // O ultrassônico sobe mesmo em modo simulado: assim dá para comparar a
    // leitura real com a do modelo lado a lado, que é como se descobre que o
    // modelo está mentindo antes de confiar nele.
    if (ultra_iniciar() != ESP_OK) ESP_LOGW(TAG, "hc-sr04 não iniciou");
    if (ads_iniciar(hal_bus, 0x48) != ESP_OK) ESP_LOGW(TAG, "ads1115 não iniciou");
    // Também sobe em modo simulado, pelo mesmo motivo do ultrassônico: comparar a
    // sonda real com o modelo lado a lado é como se descobre que o modelo mente.
    if (max_iniciar() != ESP_OK) ESP_LOGW(TAG, "max31865 não iniciou");
    ESP_LOGI(TAG, "modo=%s", s_modo == HAL_SIM ? "SIMULADO" : "real");
}

void hal_set_modo(hal_modo_t m)
{
    if (m == s_modo) return;
    // Trocar de modo com atuador ligado deixaria a saída anterior congelada no
    // hardware que está saindo. Zera antes.
    hal_sai_t zero = { 0 };
    hal_escrever(&zero);
    s_modo = m;
    ESP_LOGW(TAG, "modo -> %s", m == HAL_SIM ? "SIMULADO" : "REAL");
}

hal_modo_t hal_modo(void) { return s_modo; }

void hal_ler(hal_ent_t *e)
{
    if (s_modo == HAL_SIM) sim_ler(e);
    else real_ler(e);
}

void hal_escrever(const hal_sai_t *s)
{
    s_saida = *s;
    if (s_modo == HAL_REAL) real_escrever(s);
}

void hal_passo(float dt_s)
{
    if (s_modo == HAL_SIM) sim_passo(dt_s, &s_saida);
}
