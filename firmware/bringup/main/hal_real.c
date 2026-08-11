// HAL real + o despacho entre real e simulado.
//
// **Estado dos periféricos (11/08/2026):** temperatura, pressão e nível estão
// ligados de verdade — MAX31865, ADS1115 e HC-SR04. Faltam a célula de carga
// (amplificador não decidido) e os dois atuadores, que só entram com a máquina
// aberta.
//
// Cada leitura ausente devolve `falha_* = true` em vez de zero — porque zero é um
// número plausível, e número plausível vindo de sensor inexistente é o modo de
// falha que mais custou nesta bancada (o ToF lendo 38 cm com status "válido").
//
// Trocar para hardware é preencher as funções marcadas com PENDENTE. A lógica de
// máquina não muda uma linha.

#include "hal.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
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

static void real_escrever(const hal_sai_t *s)
{
    (void)s;
    // PENDENTE — SSR no GPIO6 por janela lenta de PWM, e o PSM: ISR de zero-cross
    // no GPIO4 (em IRAM) decidindo passar ou pular cada semiciclo no GPIO5.
    //
    // ⚠️ Enquanto isto for vazio, ligar em HAL_REAL com a máquina na tomada
    // significa que os atuadores NÃO respondem. Estado seguro é o que já está:
    // saída desenergizada.
}

// ---------------------------------------------------------------- despacho

static i2c_master_bus_handle_t hal_bus;

void hal_set_bus(void *bus) { hal_bus = (i2c_master_bus_handle_t)bus; }

void hal_iniciar(void)
{
    sim_iniciar();
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
