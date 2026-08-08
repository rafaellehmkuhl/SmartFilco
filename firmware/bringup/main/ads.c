#include "ads.h"

#include <string.h>

#include "esp_log.h"

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
//   7-5   DR      110 = 475 SPS
//   4     COMP_MODE 0 · 3-2 pol/lat 00 · 1-0 COMP_QUE 11 = comparador desligado
#define CFG_MUX_A0 (0x4 << 12)
#define CFG_PGA_6V (0x0 << 9)
#define CFG_CONTINUO (0x0 << 8)
#define CFG_475SPS (0x6 << 5)
#define CFG_SEM_COMP (0x3)
#define CONFIG (0x8000 | CFG_MUX_A0 | CFG_PGA_6V | CFG_CONTINUO | CFG_475SPS | CFG_SEM_COMP)

// ±6,144 V em 15 bits de sinal
#define LSB_V (6.144f / 32768.0f)

// Faixa elétrica válida do sensor. Fora dela não é pressão fora do limite — é
// FIO: 0 V significa sensor desconectado ou sem alimentação, e 4,9 V significa
// saída em curto com o 5 V. Sensor de 0,5–4,5 V nunca entrega esses valores em
// operação, e é justamente por isso que eles servem de detector de falha.
#define V_MIN 0.30f
#define V_MAX 4.70f

static i2c_master_dev_handle_t s_dev;
static float cal_v0 = 0.5f;          // tensão a 0 bar
static float cal_esc = 13.8f / 4.0f; // bar por volt: 13,8 bar em 4,0 V de span

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

    ESP_LOGI(TAG, "ads1115 0x%02x ok · PGA ±6,144 V · 475 SPS · A0", endereco7);
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

esp_err_t ads_ler(ads_med_t *o)
{
    if (!o) return ESP_ERR_INVALID_ARG;
    if (!s_dev) { memset(o, 0, sizeof(*o)); return ESP_ERR_INVALID_STATE; }

    uint8_t r = REG_CONV, b[2];
    esp_err_t e = i2c_master_transmit_receive(s_dev, &r, 1, b, 2, TIMEOUT_MS);
    if (e != ESP_OK) { memset(o, 0, sizeof(*o)); return e; }

    o->cru = (int16_t)(b[0] << 8 | b[1]);
    o->volts = (float)o->cru * LSB_V;
    o->valida = (o->volts >= V_MIN && o->volts <= V_MAX);
    // Converte mesmo quando inválida: o número serve para depurar, e quem decide
    // o que fazer com ele é a camada de cima, olhando `valida`.
    o->bar = (o->volts - cal_v0) * cal_esc;
    return ESP_OK;
}
