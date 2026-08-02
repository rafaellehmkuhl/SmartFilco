#include "rtc.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "rtc";

#define I2C_HZ 400000
#define TIMEOUT_MS 100

#define REG_TEMPO 0x00   // 7 bytes: seg, min, hora, dia-da-semana, data, mês, ano
#define REG_STATUS 0x0F
#define REG_TEMP 0x11    // 2 bytes: inteiro com sinal + 2 bits de fração
#define OSF_BIT 0x80

static i2c_master_dev_handle_t s_dev;
static bool s_osf;

// O DS3231 guarda tudo em BCD: 0x59 são cinquenta e nove, não oitenta e nove.
static uint8_t de_bcd(uint8_t b) { return (uint8_t)((b >> 4) * 10 + (b & 0x0F)); }
static uint8_t para_bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static esp_err_t ler(uint8_t reg, uint8_t *dst, size_t n)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_dev, &reg, 1, dst, n, TIMEOUT_MS);
}

static esp_err_t escrever(uint8_t reg, const uint8_t *src, size_t n)
{
    if (!s_dev || n > 8) return ESP_ERR_INVALID_ARG;
    uint8_t buf[9];
    buf[0] = reg;
    memcpy(buf + 1, src, n);
    return i2c_master_transmit(s_dev, buf, n + 1, TIMEOUT_MS);
}

esp_err_t rtc_iniciar(i2c_master_bus_handle_t bus, uint8_t endereco7)
{
    if (!bus) return ESP_ERR_INVALID_ARG;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = endereco7,
        .scl_speed_hz = I2C_HZ,
    };
    esp_err_t e = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (e != ESP_OK) return e;

    uint8_t st = 0;
    e = ler(REG_STATUS, &st, 1);
    if (e != ESP_OK) {
        s_dev = NULL;
        return e;
    }
    s_osf = (st & OSF_BIT) != 0;
    ESP_LOGI(TAG, "ds3231 ok, status=0x%02x%s", st,
             s_osf ? " — OSF DE PÉ, a hora guardada não vale" : "");
    return ESP_OK;
}

bool rtc_perdeu_hora(void) { return s_osf; }

esp_err_t rtc_limpar_osf(void)
{
    uint8_t st = 0;
    esp_err_t e = ler(REG_STATUS, &st, 1);
    if (e != ESP_OK) return e;
    st &= (uint8_t)~OSF_BIT;
    e = escrever(REG_STATUS, &st, 1);
    if (e == ESP_OK) s_osf = false;
    return e;
}

esp_err_t rtc_ler(struct tm *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    uint8_t b[7];
    esp_err_t e = ler(REG_TEMPO, b, sizeof(b));
    if (e != ESP_OK) return e;

    memset(out, 0, sizeof(*out));
    out->tm_sec = de_bcd(b[0] & 0x7F);
    out->tm_min = de_bcd(b[1] & 0x7F);
    // bit 6 marca modo 12 h. Gravamos sempre em 24 h, mas um chip que veio de outro
    // projeto pode estar em 12 h — tratar em vez de devolver hora errada em silêncio.
    if (b[2] & 0x40) {
        uint8_t h = de_bcd(b[2] & 0x1F) % 12;
        out->tm_hour = (b[2] & 0x20) ? h + 12 : h;   // bit 5 = PM
    } else {
        out->tm_hour = de_bcd(b[2] & 0x3F);
    }
    out->tm_mday = de_bcd(b[4] & 0x3F);
    out->tm_mon = de_bcd(b[5] & 0x1F) - 1;           // struct tm conta de 0
    out->tm_year = de_bcd(b[6]) + 100;               // struct tm conta desde 1900
    out->tm_isdst = 0;
    return ESP_OK;
}

esp_err_t rtc_gravar(const struct tm *t)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    uint8_t b[7] = {
        para_bcd((uint8_t)t->tm_sec),
        para_bcd((uint8_t)t->tm_min),
        para_bcd((uint8_t)t->tm_hour),           // bit 6 em zero = 24 h
        (uint8_t)(t->tm_wday + 1),               // o DS3231 conta 1..7
        para_bcd((uint8_t)t->tm_mday),
        para_bcd((uint8_t)(t->tm_mon + 1)),
        para_bcd((uint8_t)(t->tm_year % 100)),
    };
    esp_err_t e = escrever(REG_TEMPO, b, sizeof(b));
    if (e != ESP_OK) return e;
    // Gravar a hora sem baixar a OSF deixa o chip dizendo "perdi a hora" com a hora
    // certa dentro — o aviso viraria ruído e ninguém olharia mais para ele.
    return rtc_limpar_osf();
}

esp_err_t rtc_temp_c100(int16_t *c100)
{
    if (!c100) return ESP_ERR_INVALID_ARG;
    uint8_t b[2];
    esp_err_t e = ler(REG_TEMP, b, sizeof(b));
    if (e != ESP_OK) return e;
    // byte alto com sinal, 2 bits altos do byte baixo = quartos de grau
    *c100 = (int16_t)((int8_t)b[0] * 100 + (b[1] >> 6) * 25);
    return ESP_OK;
}
