#include "ads.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ads";

#define REG_CONV 0x00
#define REG_CONF 0x01
#define I2C_HZ 400000
#define TIMEOUT_MS 100

// Config de 16 bits, montada por campo para dar para conferir bit a bit:
//   15    OS        1 = inicia conversão (ignorado em modo contínuo)
//   14-12 MUX     100 = A0 contra GND (single-ended)
//   11-9  PGA     000 = ±6,144 V  ← obrigatório, ver o header
//   8     MODE      0 = CONTÍNUO
//   7-5   DR      111 = 860 SPS (a média móvel é quem filtra, ver abaixo)
//   4     COMP_MODE 0 · 3-2 pol/lat 00 · 1-0 COMP_QUE 11 = comparador desligado
#define CFG_MUX_A0 (0x4 << 12)
#define CFG_PGA_6V (0x0 << 9)
#define CFG_CONTINUO (0x0 << 8)
#define CFG_860SPS (0x7 << 5)
#define CFG_SEM_COMP (0x3)
#define CONFIG (0x8000 | CFG_MUX_A0 | CFG_PGA_6V | CFG_CONTINUO | CFG_860SPS | CFG_SEM_COMP)

// ±6,144 V em 15 bits de sinal
#define LSB_V (6.144f / 32768.0f)

// Faixa elétrica válida do sensor. Fora dela não é pressão fora do limite — é
// FIO: 0 V significa sensor desconectado ou sem alimentação, e 4,9 V significa
// saída em curto com o 5 V. Sensor de 0,5–4,5 V nunca entrega esses valores em
// operação, e é justamente por isso que eles servem de detector de falha.
#define V_MIN 0.30f
#define V_MAX 4.70f

// ---------------------------------------------------------------- filtro
//
// MÉDIA MÓVEL, não mediana — e o tamanho da janela não é gosto, é cálculo.
//
// O ruído dominante aqui é a **ondulação de 60 Hz da bomba** ([docs/05] e
// [docs/07]): é periódico, não são outliers. Mediana não faz nada contra
// oscilação simétrica; média com a janela certa a **anula por construção**.
//
// 25 amostras a 500 Hz = janela de 50 ms = exatamente **3 ciclos de 60 Hz**.
// Uma média que cobre um número inteiro de ciclos soma cada ciclo completo e o
// cancela — no papel dá −240 dB, na prática o que sobra é a deriva da rede e do
// relógio. Os 120 Hz do PSM caem junto, por serem harmônica.
//
// ⚠️ **Por que aqui e não no laço de controle.** O laço roda a 100 Hz. Amostrar
// 60 Hz a 100 Hz REBATE (aliasing) a ondulação para 40 Hz, e depois disso nenhum
// filtro a remove — ela vira um sinal legítimo de 40 Hz. A média tem que morar
// na taxa do sensor, antes do rebatimento. É o mesmo motivo pelo qual a task
// existe em vez de a gente ler mais vezes dentro do laço.
#define PERIODO_MS 2      // 500 Hz; o tick do FreeRTOS é 1 kHz, então bate exato
#define JANELA 25         // 50 ms = 3 ciclos de 60 Hz

static i2c_master_dev_handle_t s_dev;
static float jan[JANELA];
static uint8_t jan_n, jan_i;
static float s_filtrado;   // volts, já com a média aplicada
static float s_cru;        // volts da última conversão, para a tela de debug
static float cal_v0 = 0.5f;          // tensão a 0 bar
static float cal_esc = 13.8f / 4.0f; // bar por volt: 13,8 bar em 4,0 V de span

static void tarefa_ads(void *arg);
static TaskHandle_t s_h;
static TaskHandle_t s_task;

esp_err_t ads_iniciar(i2c_master_bus_handle_t bus, uint8_t endereco7)
{
    if (!bus) return ESP_ERR_INVALID_ARG;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = endereco7,
        .scl_speed_hz = I2C_HZ,
    };
    esp_err_t e = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (e != ESP_OK) return e;

    uint8_t w[3] = { REG_CONF, (uint8_t)(CONFIG >> 8), (uint8_t)CONFIG };
    e = i2c_master_transmit(s_dev, w, sizeof(w), TIMEOUT_MS);
    if (e != ESP_OK) { s_dev = NULL; return e; }

    // Lê de volta: escrita aceita não prova configuração aplicada, e config
    // errada aqui significa PGA errado, que satura em 4,5 V sem avisar.
    uint8_t r = REG_CONF, b[2];
    e = i2c_master_transmit_receive(s_dev, &r, 1, b, 2, TIMEOUT_MS);
    if (e != ESP_OK) { s_dev = NULL; return e; }
    uint16_t lido = (uint16_t)(b[0] << 8 | b[1]);

    // O bit 15 lido é o status de "conversão pronta", não o que escrevemos —
    // comparar sem mascarar dá falso negativo eterno.
    if ((lido & 0x7FFF) != (CONFIG & 0x7FFF)) {
        ESP_LOGE(TAG, "config não gravou: escrevi 0x%04x, li 0x%04x", CONFIG, lido);
        s_dev = NULL;
        return ESP_ERR_INVALID_RESPONSE;
    }

    xTaskCreatePinnedToCore(tarefa_ads, "ads", 3072, NULL, 5, &s_h, 0);

    ESP_LOGI(TAG, "ads1115 0x%02x ok · PGA ±6,144 V · 860 SPS · média de %d @ %d Hz",
             endereco7, JANELA, 1000 / PERIODO_MS);
    return ESP_OK;
}

void ads_calibrar(float v0, float bar_por_volt)
{
    cal_v0 = v0;
    cal_esc = bar_por_volt;
    ESP_LOGI(TAG, "calibração: 0 bar em %.3f V, %.3f bar/V", v0, bar_por_volt);
}

void ads_get_cal(float *v0, float *esc)
{
    if (v0) *v0 = cal_v0;
    if (esc) *esc = cal_esc;
}

static esp_err_t ads_amostrar(int16_t *cru)
{
    uint8_t r = REG_CONV, b[2];
    esp_err_t e = i2c_master_transmit_receive(s_dev, &r, 1, b, 2, TIMEOUT_MS);
    if (e != ESP_OK) return e;
    *cru = (int16_t)(b[0] << 8 | b[1]);
    return ESP_OK;
}

static void tarefa_ads(void *arg)
{
    TickType_t ult = xTaskGetTickCount();
    while (1) {
        int16_t cru;
        if (ads_amostrar(&cru) == ESP_OK) {
            float v = (float)cru * LSB_V;
            s_cru = v;
            jan[jan_i] = v;
            jan_i = (uint8_t)((jan_i + 1) % JANELA);
            if (jan_n < JANELA) jan_n++;
            float soma = 0.0f;
            for (uint8_t k = 0; k < jan_n; k++) soma += jan[k];
            s_filtrado = soma / (float)jan_n;
        }
        vTaskDelayUntil(&ult, pdMS_TO_TICKS(PERIODO_MS));
    }
}

esp_err_t ads_ler(ads_med_t *o)
{
    if (!o) return ESP_ERR_INVALID_ARG;
    if (!s_dev) { memset(o, 0, sizeof(*o)); return ESP_ERR_INVALID_STATE; }

    o->volts = s_filtrado;
    o->volts_cru = s_cru;
    o->cru = (int16_t)(s_filtrado / LSB_V);
    // A validade olha o valor CRU, não o filtrado: sensor que se soltou leva
    // 50 ms para a média descer, e nesse meio tempo o filtrado ainda parece
    // plausível. Falha elétrica tem que aparecer na hora.
    o->valida = (s_cru >= V_MIN && s_cru <= V_MAX);
    o->bar = (o->volts - cal_v0) * cal_esc;
    return ESP_OK;
}

// A 500 Hz esta task é a maior fonte de interrupção da placa. Durante o OTA ela
// precisa calar: a escrita em flash desabilita cache e interrupção em janelas
// curtas, e com tráfego de I2C denso o watchdog de interrupção (TG1, 800 ms)
// derruba a gravação no meio. Medido: OTA morria em 135 KB com os sensores
// rodando e passava inteiro com eles parados.
void ads_pausar(bool p)
{
    if (!s_h) return;
    if (p) vTaskSuspend(s_h); else vTaskResume(s_h);
}
