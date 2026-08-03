// HAL real + o despacho entre real e simulado.
//
// ⚠️ **Nada aqui está ligado ainda.** PT100/MAX31865 e o sensor de 200 PSI chegam
// 05/08; a célula de carga não foi comprada. Cada leitura ausente devolve
// `falha_* = true` em vez de zero — porque zero é um número plausível, e número
// plausível vindo de sensor inexistente é o modo de falha que mais custou nesta
// bancada (o ToF lendo 38 cm com status "válido").
//
// Trocar para hardware é preencher as funções marcadas com PENDENTE. A lógica de
// máquina não muda uma linha.

#include "hal.h"
#include "esp_log.h"

static const char *TAG = "hal";

// implementadas em hal_sim.c
void sim_iniciar(void);
void sim_passo(float dt_s, const hal_sai_t *s);
void sim_ler(hal_ent_t *e);

static hal_modo_t s_modo = HAL_SIM;
static hal_sai_t s_saida;

// ---------------------------------------------------------------- real

static void real_ler(hal_ent_t *e)
{
    // PENDENTE 05/08 — MAX31865 no SPI, CS no GPIO10. Ler o registrador de FAULT,
    // não só o valor: a detecção de sonda aberta/curta é por hardware.
    e->t_boiler = 0.0f;
    e->falha_temp = true;

    // PENDENTE 05/08 — ADS1115 no I²C, canal A0, PGA ±6,144 V.
    // P(bar) = (V − 0,5) / 4,0 × 13,8. Fora de 0,4–4,6 V = falha elétrica.
    e->pressao = 0.0f;
    e->falha_press = true;

    // PENDENTE — célula de carga. Amplificador ainda não decidido
    // ([docs/02]): NAU7802 no I²C ou HX711 em 2 GPIOs.
    e->massa = 0.0f;
    e->falha_massa = true;

    // PENDENTE — HC-SR04, TRIG no 15 e ECHO no 16 (⚠️ ECHO é 5 V, precisa de
    // divisor ou do level shifter).
    e->nivel_mm = 0.0f;
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

void hal_iniciar(void)
{
    sim_iniciar();
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
