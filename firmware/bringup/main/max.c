#include "max.h"

#include <assert.h>
#include <math.h>
#include <string.h>

#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "max";

// Mesmos pinos da docs/02, iguais aos do sd.c — o barramento é o mesmo, só o CS
// muda.
#define PIN_MOSI 11
#define PIN_SCK 12
#define PIN_MISO 13
#define PIN_CS 10
#define SPI_HZ (1 * 1000 * 1000)   // teto do chip é 5 MHz; 1 basta e sobra em jumper

#define REG_CONFIG 0x00
#define REG_RTD 0x01
#define REG_FALHA 0x07
#define ESCRITA 0x80               // endereço | 0x80 = escrita

// Configuração de 8 bits, campo a campo para dar para conferir bit a bit:
//   D7 VBIAS      1 = corrente de polarização sempre ligada
//   D6 AUTO       1 = converte sozinho, ~21 ms por conversão
//   D5 1-SHOT     0 = não usamos disparo manual
//   D4 3-FIOS     1 ← tem que casar com o jumper da placa
//   D3-D2 falha  00 = sem ciclo automático de detecção (os bits passivos do 0x07
//                     continuam sendo atualizados a cada conversão, que é o que
//                     pega sonda aberta)
//   D1 limpa      escrito só no início; auto-zera
//   D0 filtro     0 = 60 Hz ← a rede daqui. 1 seria 50 Hz.
#define CFG_VBIAS 0x80
#define CFG_AUTO 0x40
#define CFG_3FIOS 0x10
#define CFG_LIMPA_FALHA 0x02
#define CONFIG (CFG_VBIAS | CFG_AUTO | CFG_3FIOS)

// VBIAS ligado o tempo todo aquece a própria sonda? Com Rref de 430 Ω a corrente
// de polarização fica em ~100 µA, o que num PT100 dissipa 1 µW. Contra a inércia
// de um boiler de alumínio isso é zero — e o preço de não ter que orquestrar
// disparo manual e espera de 65 ms dentro de um laço de controle é alto demais.

// Callendar–Van Dusen, ramo positivo.
#define RTD_A 3.9083e-3f
#define RTD_B -5.775e-7f
#define R0 100.0f                  // PT100

// Faixa ELÉTRICA plausível para um PT100, não faixa de café: fora disto não é
// temperatura estranha, é fiação. Dá ~-101 °C a ~+558 °C — larga de propósito,
// para que quem reclame de temperatura fora do café seja a máquina de estados,
// com o contexto dela, e não este driver.
#define OHM_MIN 60.0f
#define OHM_MAX 300.0f

#define PERIODO_MS 100             // 10 Hz; o chip converte a ~48 Hz e o boiler
                                   // tem constante de tempo de minutos

static spi_device_handle_t s_dev;
static volatile bool s_pronto;
static float s_rref = 430.0f;
static float s_offset_c = 0.0f;

static volatile float s_celsius;
static volatile float s_ohms;
static volatile uint16_t s_cru;
static volatile bool s_valida;
static volatile uint8_t s_falha;
static const char *volatile s_falha_txt = "";

static void tarefa_max(void *arg);

// ---------------------------------------------------------------- registradores

static esp_err_t reg_ler(uint8_t reg, uint8_t *dst, size_t n)
{
    // SPI_TRANS_USE_RXDATA e no máximo 4 bytes: evita exigir buffer alinhado para
    // DMA por causa de uma leitura de 2 bytes.
    assert(n <= 4);
    spi_transaction_t t = {
        .addr = reg,
        .length = n * 8,
        .rxlength = n * 8,
        .flags = SPI_TRANS_USE_RXDATA,
    };
    esp_err_t e = spi_device_transmit(s_dev, &t);
    if (e == ESP_OK) memcpy(dst, t.rx_data, n);
    return e;
}

static esp_err_t reg_escrever(uint8_t reg, uint8_t v)
{
    spi_transaction_t t = {
        .addr = reg | ESCRITA,
        .length = 8,
        .flags = SPI_TRANS_USE_TXDATA,
        .tx_data = { v },
    };
    return spi_device_transmit(s_dev, &t);
}

// ---------------------------------------------------------------- conversão

// R = R0(1 + A·T + B·T²), resolvida para T. O ramo positivo vale de 0 °C para
// cima; abaixo disso a norma pede um polinômio de 5ª ordem que esta máquina nunca
// vai precisar — em São José dos Campos, boiler abaixo de zero é sonda quebrada, e
// quem responde por isso é o registrador de falha.
static float ohms_para_c(float r)
{
    float disc = RTD_A * RTD_A - 4.0f * RTD_B * (1.0f - r / R0);
    if (disc < 0.0f) return -999.0f;
    return (-RTD_A + sqrtf(disc)) / (2.0f * RTD_B);
}

static const char *falha_nome(uint8_t f)
{
    // Ordem por poder de diagnóstico, não por número do bit: quem olha quer saber
    // o que fazer, e "sonda aberta" manda conferir borne, enquanto os bits de
    // limiar mandam conferir número.
    if (f & 0x04) return "sub/sobretensão";   // D2 OVUV
    if (f & 0x20) return "sonda aberta";      // D5 REFIN- > 0,85·VBIAS
    if (f & 0x18) return "FORCE- aberto";     // D4/D3
    if (f & 0x80) return "acima do limite";   // D7
    if (f & 0x40) return "abaixo do limite";  // D6
    return f ? "falha desconhecida" : "";
}

// ---------------------------------------------------------------- autoteste
//
// A conversão é a única lógica não trivial daqui, e ela erra CALADA: um sinal
// trocado devolve temperatura plausível. Os três pontos abaixo são tabelados da
// PT100 — se algum dia alguém "simplificar" a fórmula, isto trava o boot.

static void autoteste(void)
{
    assert(fabsf(ohms_para_c(100.00f) - 0.0f) < 0.1f);      // 0 °C por definição
    assert(fabsf(ohms_para_c(138.51f) - 100.0f) < 0.2f);    // tabela IEC 60751
    assert(fabsf(ohms_para_c(134.71f) - 90.0f) < 0.2f);     // perto do ponto de trabalho
    assert(ohms_para_c(138.51f) > ohms_para_c(134.71f));    // monotônica
}

// ---------------------------------------------------------------- vida

esp_err_t max_iniciar(void)
{
    autoteste();

    spi_device_interface_config_t dev = {
        .mode = 1,                 // CPOL=0, CPHA=1 — o cartão SD é modo 0, e o
                                   // IDF troca por transação
        .clock_speed_hz = SPI_HZ,
        .spics_io_num = PIN_CS,
        .address_bits = 8,
        .queue_size = 1,
    };

    // Entrar primeiro, criar só se não houver barramento. Na ordem inversa — que
    // é a intuitiva — o IDF loga um ERRO vermelho de "SPI bus already
    // initialized" em todo boot em que o sd.c chegou antes, e log vermelho que
    // não significa nada ensina a ignorar log vermelho.
    esp_err_t e = spi_bus_add_device(SPI2_HOST, &dev, &s_dev);
    if (e == ESP_ERR_INVALID_STATE) {
        spi_bus_config_t bus = {
            .mosi_io_num = PIN_MOSI,
            .miso_io_num = PIN_MISO,
            .sclk_io_num = PIN_SCK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4000,
        };
        e = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(e));
            return e;
        }
        e = spi_bus_add_device(SPI2_HOST, &dev, &s_dev);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(e));
        return e;
    }

    e = reg_escrever(REG_CONFIG, CONFIG | CFG_LIMPA_FALHA);
    if (e != ESP_OK) { s_dev = NULL; return e; }

    // Lê de volta pelo mesmo motivo do ADS1115: escrita aceita não prova
    // configuração aplicada. Aqui o que está em jogo é o bit de 3 fios — errá-lo
    // não dá erro nenhum, dá temperatura errada e firme.
    uint8_t lido = 0;
    e = reg_ler(REG_CONFIG, &lido, 1);
    if (e != ESP_OK) { s_dev = NULL; return e; }
    if (lido != CONFIG) {
        ESP_LOGE(TAG, "config não gravou: escrevi 0x%02x, li 0x%02x%s",
                 CONFIG, lido, lido == 0xFF || lido == 0x00 ? " (chip ausente?)" : "");
        s_dev = NULL;
        return ESP_ERR_INVALID_RESPONSE;
    }

    s_pronto = true;
    xTaskCreatePinnedToCore(tarefa_max, "max", 3072, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "max31865 ok · 3 fios · 60 Hz · Rref %.0f Ω · %d Hz",
             (double)s_rref, 1000 / PERIODO_MS);
    return ESP_OK;
}

void max_calibrar(float rref, float offset_c)
{
    if (rref > 100.0f && rref < 5000.0f) s_rref = rref;
    s_offset_c = offset_c;
    ESP_LOGI(TAG, "calibração: Rref %.1f Ω, offset %+.2f °C", (double)s_rref, (double)s_offset_c);
}

void max_get_cal(float *rref, float *offset_c)
{
    if (rref) *rref = s_rref;
    if (offset_c) *offset_c = s_offset_c;
}

bool max_pronto(void) { return s_pronto; }

static void tarefa_max(void *arg)
{
    TickType_t ult = xTaskGetTickCount();
    while (1) {
        uint8_t b[2];
        if (reg_ler(REG_RTD, b, 2) == ESP_OK) {
            uint16_t bruto = (uint16_t)(b[0] << 8 | b[1]);
            // O bit 0 do registrador RTD é sinalizador de falha, não valor. Quem
            // esquece de deslocar lê o dobro da resistência e uma temperatura
            // altíssima e crível.
            bool sinalizou = bruto & 1;
            uint16_t cru = bruto >> 1;
            float ohms = (float)cru * s_rref / 32768.0f;

            uint8_t f = 0;
            if (sinalizou) {
                reg_ler(REG_FALHA, &f, 1);
                // Limpa para o próximo ciclo poder relatar de novo. Sem isso a
                // falha fica latchada e uma sonda que voltou continua acusada.
                reg_escrever(REG_CONFIG, CONFIG | CFG_LIMPA_FALHA);
            }

            bool eletrica_ok = (ohms >= OHM_MIN && ohms <= OHM_MAX);
            s_cru = cru;
            s_ohms = ohms;
            s_celsius = ohms_para_c(ohms) + s_offset_c;
            s_falha = f;
            s_falha_txt = f ? falha_nome(f) : (eletrica_ok ? "" : "fora de faixa");
            s_valida = (f == 0) && eletrica_ok;
        }
        vTaskDelayUntil(&ult, pdMS_TO_TICKS(PERIODO_MS));
    }
}

esp_err_t max_ler(max_med_t *o)
{
    if (!o) return ESP_ERR_INVALID_ARG;
    if (!s_pronto) { memset(o, 0, sizeof(*o)); o->falha_txt = ""; return ESP_ERR_INVALID_STATE; }

    o->celsius = s_celsius;
    o->ohms = s_ohms;
    o->cru = s_cru;
    o->valida = s_valida;
    o->falha = s_falha;
    o->falha_txt = s_falha_txt;
    return ESP_OK;
}
