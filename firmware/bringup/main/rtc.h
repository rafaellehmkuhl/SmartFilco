// DS3231 no barramento i2c_master que já existe.
//
// Escrito à mão, sem componente: são 7 registradores em BCD e um de status. Puxar
// dependência para isso custaria mais que o código — e as duas vezes que puxamos
// componente nesta bancada (BME280 que era 680, ULP que não mede distância) custaram
// mais caro que escrever.
//
// ⚠️ Isto é BANCADA. O projeto usa NTP e a docs/05 removeu o `RTCModule` de
// propósito: sem rede não há requisito de log correto. O que este módulo prova é o
// caminho inverso — que dá para o RTC sustentar a hora quando a rede cai — para a
// decisão poder ser revista com dado em vez de suposição.

#pragma once

#include <stdbool.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t rtc_iniciar(i2c_master_bus_handle_t bus, uint8_t endereco7);

esp_err_t rtc_ler(struct tm *out);
esp_err_t rtc_gravar(const struct tm *t);

// Sensor interno do DS3231 (existe para compensar o cristal). ±3 °C de exatidão —
// não serve para medir ambiente, serve para conferir que o chip está vivo e para
// comparar com o BME680.
esp_err_t rtc_temp_c100(int16_t *c100);

// ⚠️ **A flag que decide se a hora vale.** OSF = Oscillator Stop Flag, bit 7 do
// registrador 0x0F. Liga sozinha sempre que o oscilador parou — bateria acabou,
// primeira energização, chip trocado. Enquanto ela estiver de pé, o DS3231 devolve
// uma hora perfeitamente formatada e completamente inventada.
//
// É o mesmo problema do ToF lendo 38 cm com status "válido": número plausível é o
// modo de falha mais caro. Só que aqui o próprio chip avisa — basta ler.
bool rtc_perdeu_hora(void);
esp_err_t rtc_limpar_osf(void);
