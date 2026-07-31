// VL53L1X sobre a ULD da ST, no barramento i2c_master que já existe.
//
// Por que este componente existe, em uma frase: o `grrtzm/vl53l1x_library` que
// usávamos é o port da API **ULP** (Ultra Low Power), feita para detectar presença
// com limiar — ela não mede distância. Medido em 31/07/2026, a escala dela varia de
// 7,33x a 1,87x conforme o macro timing e nunca chega em 1. A **ULD** é a API de
// ranging, e é a que tem modo de distância, orçamento de tempo e as calibrações de
// offset e crosstalk.
//
// A alternativa pronta (`saleca/vl53l1x_uld_esp_wrapper`) traz a ULD certa mas cria
// o próprio barramento a partir de SDA/SCL, o que colidiria com o bus compartilhado
// com o BME680. Daqui saiu só o `core/` da ST; a plataforma é nossa.
//
// ⚠️ Um sensor só. A ULD passa o endereço no parâmetro `dev`, mas aqui guardamos um
// handle único. Para um segundo VL53L1X, virar um array indexado por `dev`.

#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t mm;        // distância (via VL53L1X_GetResult)
    uint16_t mm_direto; // a mesma, via VL53L1X_GetDistance — contraprova
    uint8_t  status;    // range status da ST; 0 = válida
    uint16_t sigma_mm;  // desvio-padrão estimado da medida
    uint16_t sinal_kcps;
    uint16_t ambiente_kcps;
} tof_leitura_t;

// Registra a plataforma, acorda o sensor e configura para curta distância.
// `endereco7` é 7-bit (0x29 de fábrica).
esp_err_t tof_iniciar(i2c_master_bus_handle_t bus, uint8_t endereco7);

// Modo curto: alcance até ~1,3 m, mais imune a luz ambiente. É o certo para
// reservatório (4–25 cm) e para presença de copo. `orcamento_ms` de 20 a 1000;
// mais tempo = menos ruído. `periodo_ms` não pode ser menor que o orçamento.
esp_err_t tof_config_curta(uint16_t orcamento_ms, uint16_t periodo_ms);

// Diagnóstico: lê de volta o que o sensor REALMENTE está usando, mais o valor de
// calibração do oscilador — o relógio de que dependem tanto o período entre
// medidas quanto a conversão interna de contagem para milímetro.
esp_err_t tof_dump(uint16_t *osc, uint16_t *modo, uint16_t *orcamento_ms,
                   uint16_t *periodo_ms);

// Período entre medidas, em ms. Exposto sozinho para poder medir a taxa real
// contra a pedida: se divergirem por um fator, o relógio do sensor está errado.
esp_err_t tof_set_periodo_ms(uint16_t ms);

// Orçamento de tempo por medida. Mais longo = mais imune a luz ambiente, menos
// taxa. A ULD só aceita 15, 20, 33, 50, 100, 200 ou 500 ms.
esp_err_t tof_set_orcamento_ms(uint16_t ms);

esp_err_t tof_start(void);
esp_err_t tof_stop(void);

// Espera a medida ficar pronta (até `timeout_ms`), lê e libera o sensor para a
// próxima. Devolve ESP_ERR_TIMEOUT se não ficou pronta.
esp_err_t tof_ler(tof_leitura_t *out, uint32_t timeout_ms);

// Offset em mm somado a toda leitura. É a calibração NORMAL de um ToF: corrige
// DESLOCAMENTO, não escala.
//
// ⚠️ Ele NÃO vive na RAM do ESP32 — mora no registrador
// ALGO__PART_TO_PART_RANGE_OFFSET_MM (0x001E) do próprio sensor. Verificado em
// 31/07/2026: sobreviveu a regravação + reboot do ESP32. O VL53L1X_SensorInit
// escreve a configuração default a partir de 0x2D, então não passa por cima dele.
// Só some quando o SENSOR perde alimentação. Consequência prática: uma calibração
// ruim fica lá, silenciosa, até alguém escrever por cima.
esp_err_t tof_set_offset_mm(int16_t mm);
esp_err_t tof_get_offset_mm(int16_t *mm);

// Procedimento da ST: alvo plano a `distancia_real_mm` (o manual sugere 140 mm,
// refletância 88%), sensor parado. Bloqueia alguns segundos e grava o offset.
esp_err_t tof_calibrar_offset(uint16_t distancia_real_mm, int16_t *achado);

#ifdef __cplusplus
}
#endif
