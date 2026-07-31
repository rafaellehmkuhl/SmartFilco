// Camada de plataforma da ULD sobre o i2c_master do IDF 5.x.
//
// A ULD da ST fala com o mundo por oito funções (`VL53L1_RdByte`, `WrWord`, ...).
// O `vl53l1_platform.c` que veio junto já despacha por ponteiro de função — só é
// preciso apontar os `g_vl53l1x_*_ptr` para cá antes de tocar no sensor.
//
// Detalhe do barramento: o VL53L1X endereça registrador em **16 bits big-endian**,
// então toda transação começa com dois bytes de índice.

#include "tof.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "VL53L1X_api.h"
#include "VL53L1X_calibration.h"
#include "vl53l1_platform.h"

static const char *TAG = "tof";

#define I2C_HZ 400000
#define I2C_TIMEOUT_MS 100
// A maior escrita que a ULD faz é dword (4 B). Índice de 16 bits na frente.
#define MAX_CARGA 8

static i2c_master_dev_handle_t s_dev;

// ------------------------------------------------------------------ plataforma

static int8_t p_write_multi(uint16_t dev, uint16_t idx, uint8_t *p, uint32_t n)
{
    (void)dev;
    if (!s_dev || n > MAX_CARGA) return -1;
    uint8_t buf[2 + MAX_CARGA];
    buf[0] = (uint8_t)(idx >> 8);
    buf[1] = (uint8_t)(idx & 0xFF);
    memcpy(buf + 2, p, n);
    return i2c_master_transmit(s_dev, buf, 2 + n, I2C_TIMEOUT_MS) == ESP_OK ? 0 : -1;
}

static int8_t p_read_multi(uint16_t dev, uint16_t idx, uint8_t *p, uint32_t n)
{
    (void)dev;
    if (!s_dev) return -1;
    uint8_t a[2] = { (uint8_t)(idx >> 8), (uint8_t)(idx & 0xFF) };
    // transmit_receive faz o repeated-START que o VL53L1X exige entre índice e
    // leitura. Mandar STOP no meio faz o sensor devolver lixo.
    return i2c_master_transmit_receive(s_dev, a, 2, p, n, I2C_TIMEOUT_MS) == ESP_OK ? 0 : -1;
}

static int8_t p_wr_byte(uint16_t dev, uint16_t idx, uint8_t v)
{
    return p_write_multi(dev, idx, &v, 1);
}

static int8_t p_wr_word(uint16_t dev, uint16_t idx, uint16_t v)
{
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    return p_write_multi(dev, idx, b, 2);
}

static int8_t p_wr_dword(uint16_t dev, uint16_t idx, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    return p_write_multi(dev, idx, b, 4);
}

static int8_t p_rd_byte(uint16_t dev, uint16_t idx, uint8_t *v)
{
    return p_read_multi(dev, idx, v, 1);
}

static int8_t p_rd_word(uint16_t dev, uint16_t idx, uint16_t *v)
{
    uint8_t b[2];
    int8_t e = p_read_multi(dev, idx, b, 2);
    if (e == 0) *v = ((uint16_t)b[0] << 8) | b[1];
    return e;
}

static int8_t p_rd_dword(uint16_t dev, uint16_t idx, uint32_t *v)
{
    uint8_t b[4];
    int8_t e = p_read_multi(dev, idx, b, 4);
    if (e == 0) *v = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                     ((uint32_t)b[2] << 8) | b[3];
    return e;
}

// VL53L1_WaitMs NÃO entra aqui: o vl53l1_platform.c da ST já a implementa com
// vTaskDelay, e é a única das nove que não passa por ponteiro de função.
// Redefinir dá "multiple definition" no link.

// ------------------------------------------------------------------ API

esp_err_t tof_iniciar(i2c_master_bus_handle_t bus, uint8_t endereco7)
{
    if (!bus) return ESP_ERR_INVALID_ARG;

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = endereco7,
        .scl_speed_hz = I2C_HZ,
    };
    esp_err_t e = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (e != ESP_OK) return e;

    g_vl53l1x_write_multi_ptr = p_write_multi;
    g_vl53l1x_read_multi_ptr = p_read_multi;
    g_vl53l1x_write_byte_ptr = p_wr_byte;
    g_vl53l1x_write_word_ptr = p_wr_word;
    g_vl53l1x_write_dword_ptr = p_wr_dword;
    g_vl53l1x_read_byte_ptr = p_rd_byte;
    g_vl53l1x_read_word_ptr = p_rd_word;
    g_vl53l1x_read_dword_ptr = p_rd_dword;

    // O sensor leva ~2 ms para levantar o firmware depois do power-up. Ler antes
    // disso devolve zeros que passariam por resposta válida.
    uint8_t pronto = 0;
    for (int i = 0; i < 100 && !pronto; i++) {
        if (VL53L1X_BootState(0, &pronto) != 0) return ESP_ERR_NOT_FOUND;
        if (!pronto) vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (!pronto) {
        ESP_LOGE(TAG, "não saiu do boot");
        return ESP_ERR_TIMEOUT;
    }

    uint16_t id = 0;
    if (VL53L1X_GetSensorId(0, &id) != 0) return ESP_ERR_NOT_FOUND;
    if (id != 0xEACC) {
        ESP_LOGE(TAG, "id 0x%04x, esperado 0xEACC — não é VL53L1X", id);
        return ESP_ERR_NOT_FOUND;
    }
    if (VL53L1X_SensorInit(0) != 0) return ESP_FAIL;

    ESP_LOGI(TAG, "ULD ok (id=0x%04x)", id);
    return ESP_OK;
}

esp_err_t tof_config_curta(uint16_t orcamento_ms, uint16_t periodo_ms)
{
    // O período entre medidas não pode ser menor que o orçamento, senão a ULD
    // ignora o pedido e fica no valor anterior — silenciosamente.
    if (periodo_ms < orcamento_ms) return ESP_ERR_INVALID_ARG;
    int8_t e = 0;
    e |= VL53L1X_SetDistanceMode(0, 1);                    // 1 = curta
    e |= VL53L1X_SetTimingBudgetInMs(0, orcamento_ms);
    e |= VL53L1X_SetInterMeasurementInMs(0, periodo_ms);
    return e == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t tof_dump(uint16_t *osc, uint16_t *modo, uint16_t *orcamento_ms,
                   uint16_t *periodo_ms)
{
    uint16_t v = 0;
    // 0x00DE = RESULT__OSC_CALIBRATE_VAL. A própria ULD só usa os 10 bits baixos.
    if (VL53L1_RdWord(0, 0x00DE, &v) != 0) return ESP_FAIL;
    if (osc) *osc = v & 0x3FF;
    if (modo) { uint16_t m = 0; VL53L1X_GetDistanceMode(0, &m); *modo = m; }
    if (orcamento_ms) { uint16_t t = 0; VL53L1X_GetTimingBudgetInMs(0, &t); *orcamento_ms = t; }
    if (periodo_ms) { uint16_t p = 0; VL53L1X_GetInterMeasurementInMs(0, &p); *periodo_ms = p; }
    return ESP_OK;
}

esp_err_t tof_set_periodo_ms(uint16_t ms)
{
    return VL53L1X_SetInterMeasurementInMs(0, ms) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t tof_start(void) { return VL53L1X_StartRanging(0) == 0 ? ESP_OK : ESP_FAIL; }
esp_err_t tof_stop(void) { return VL53L1X_StopRanging(0) == 0 ? ESP_OK : ESP_FAIL; }

esp_err_t tof_ler(tof_leitura_t *out, uint32_t timeout_ms)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    uint8_t pronto = 0;
    uint32_t esperou = 0;
    while (!pronto) {
        if (VL53L1X_CheckForDataReady(0, &pronto) != 0) return ESP_FAIL;
        if (pronto) break;
        if (esperou >= timeout_ms) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(5));
        esperou += 5;
    }

    VL53L1X_Result_t r;
    if (VL53L1X_GetResult(0, &r) != 0) return ESP_FAIL;
    out->mm = r.Distance;
    out->status = r.Status;
    out->sinal_kcps = r.SigPerSPAD;
    out->ambiente_kcps = r.Ambient;

    // Esta versão da ULD não expõe o sigma: o VL53L1X_Result_t traz Status,
    // Distance, Ambient, SigPerSPAD e NumSPADs, e não há VL53L1X_GetSigma().
    // O registrador existe — o próprio GetResult lê 17 bytes a partir de
    // RESULT__RANGE_STATUS (0x0089), e o sigma é o word em 0x0092. O /4 é o
    // mesmo fator que o código da ST aplica (ponto fixo).
    // Sem sigma dá para ver que a medida é ruim, mas não O QUANTO — e foi
    // justamente o sigma que denunciou o driver anterior.
    uint16_t sig_raw = 0;
    if (VL53L1_RdWord(0, 0x0092, &sig_raw) == 0) out->sigma_mm = sig_raw / 4;
    else out->sigma_mm = 0xFFFF;

    // Contraprova: mesma distância pelo outro caminho da ULD. GetResult desempacota
    // um bloco de 17 bytes; GetDistance lê o registrador direto. Se os dois baterem,
    // qualquer erro restante é de CONFIGURAÇÃO ou físico — não do desempacotamento
    // nem da camada I2C que escrevi.
    VL53L1X_GetDistance(0, &out->mm_direto);

    // Obrigatório: sem limpar a interrupção o sensor repete a mesma medida.
    return VL53L1X_ClearInterrupt(0) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t tof_set_offset_mm(int16_t mm)
{
    return VL53L1X_SetOffset(0, mm) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t tof_get_offset_mm(int16_t *mm)
{
    if (!mm) return ESP_ERR_INVALID_ARG;
    return VL53L1X_GetOffset(0, mm) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t tof_calibrar_offset(uint16_t distancia_real_mm, int16_t *achado)
{
    int16_t off = 0;
    int8_t e = VL53L1X_CalibrateOffset(0, distancia_real_mm, &off);
    if (e != 0) return ESP_FAIL;
    if (achado) *achado = off;
    ESP_LOGI(TAG, "offset calibrado: %d mm (alvo a %u mm)", off, distancia_real_mm);
    return ESP_OK;
}
