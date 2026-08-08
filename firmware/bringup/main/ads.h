// ADS1115 — sensor de pressão do grupo.
//
// Escrito à mão: são dois registradores de 16 bits. Componente para isto custaria
// mais que o código, e já levamos duas mordidas nesta bancada por confiar em
// driver de terceiro (o "BME280" que era 680, o ULP que não mede distância).
//
// ## A cadeia, e por que o PGA não é opcional
//
// Sensor de 200 PSI entrega **0,5–4,5 V** para 0–13,8 bar. O ADS1115 roda em 5 V
// para ter essa folga, e por isso o I²C dele passa por level shifter — sem ele o
// barramento de 3,3 V sobe para 5 V e leva junto o ESP32, o BME680 e o DS3231
// ([docs/02]).
//
// ⚠️ **PGA em ±6,144 V é obrigatório.** No ganho seguinte (±4,096 V) o sinal de
// 4,5 V satura e a leitura trava no fundo de escala parecendo pressão máxima —
// que é exatamente o tipo de número plausível e errado que custa caro aqui.
//
// Resolução resultante: 187,5 µV/LSB, ou ~0,65 mbar por contagem sobre os
// 13,8 bar. O ruído do sensor e a ondulação de 60 Hz da bomba limitam muito antes
// disso, então o que falta é filtro, não bit.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

typedef struct {
    float volts;      // tensão FILTRADA (média móvel de 3 ciclos de rede)
    float volts_cru;  // última conversão, sem filtro — para a tela de debug
    float bar;        // já convertida
    int16_t cru;      // contagem do ADC, para depurar sem refazer a conta
    bool  valida;     // dentro da faixa elétrica do sensor
} ads_med_t;

// `endereco7` é 7-bit: 0x48 com ADDR em GND.
esp_err_t ads_iniciar(i2c_master_bus_handle_t bus, uint8_t endereco7);

// Lê a última conversão. Modo contínuo, então não bloqueia — o laço de controle
// pega o valor mais recente sem esperar conversão.
esp_err_t ads_ler(ads_med_t *o);

// Calibração de dois pontos contra o manômetro analógico, como a docs/07 manda.
// `v0` é a tensão a 0 bar e `esc` os bar por volt. Nominais: 0,5 V e 3,45 bar/V.
void ads_calibrar(float v0, float bar_por_volt);
void ads_get_cal(float *v0, float *bar_por_volt);
