// MAX31865 + PT100 — temperatura da parede do boiler.
//
// Escrito à mão pelo mesmo motivo do ADS1115: são oito registradores e uma
// equação. Componente de terceiro custaria mais depuração do que o código, e
// esta bancada já levou duas mordidas por confiar em driver alheio — o "BME280"
// que era 680 e o ULP do ToF que não media distância.
//
// ## O que este driver assume do hardware ([bancada-esquematico])
//
// - **3 fios**, com o par de mesma cor em `F+`/`RTD+` e o solitário em `RTD−`.
//   O bit de 3 fios daqui **tem que casar com o jumper da placa**: se um disser
//   3 e o outro 2/4, a leitura não falha — sai estável e errada.
// - **Rref de 430 Ω**, que é o que faz da placa um PT100 e não um PT1000. Ele
//   entra direto na conta, então a tolerância dele **é** o erro de escala: por
//   isso `max_calibrar` existe.
// - **SPI modo 1** (CPOL=0, CPHA=1), no mesmo barramento do microSD, com `CS`
//   próprio no GPIO 10. Cada dispositivo leva o próprio clock na configuração do
//   IDF, então o cartão continua a 10 MHz enquanto este anda a 1 MHz — o teto do
//   MAX31865 é 5 MHz, e fio de protoboard não pede pressa.
//
// ## Falha é lida, não inferida
//
// O chip tem registrador de falha com detecção de sonda aberta e curto. A `hal.h`
// já diz por que isso importa: heurística sobre o valor confunde "sonda caiu" com
// "boiler frio", e são consertos opostos. Aqui a falha vem do registrador 0x07 e
// sobe até `hal_ent_t.falha_temp` sem virar palpite no caminho.
//
// ## Por que uma task, e não leitura sob demanda
//
// Mesmo argumento do ADS1115, com um agravante: este barramento é compartilhado
// com o cartão SD. Ler dentro do laço de controle significaria bloquear o laço
// enquanto o SD escreve um bloco. A task amostra a 10 Hz — boiler tem constante
// de tempo de minutos — e `max_ler` só copia o resultado.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    float celsius;      // já convertido (Callendar–Van Dusen)
    float ohms;         // resistência medida — é o que se confere com multímetro
    uint16_t cru;       // 15 bits do registrador RTD, para depurar sem refazer a conta
    bool  valida;       // sem falha de hardware e dentro da faixa elétrica plausível
    uint8_t falha;      // registrador 0x07 cru, 0 = limpo
    const char *falha_txt;  // a falha dominante em uma palavra; "" se limpo
} max_med_t;

// Sobe o barramento SPI se ninguém subiu ainda (o microSD faz o mesmo — quem
// chegar primeiro cria, o segundo aceita ESP_ERR_INVALID_STATE), configura o chip
// e confere a configuração lendo de volta.
esp_err_t max_iniciar(void);

// Copia a última amostra. Não toca no SPI, então não bloqueia o laço de controle.
esp_err_t max_ler(max_med_t *o);

// Separa "sensor ausente" de "driver não existe" na tela de debug.
bool max_pronto(void);

// Os dois ajustes que o mundo físico pede:
//   `rref`   — o valor MEDIDO do resistor de referência (nominal 430 Ω). Erra a
//              escala inteira proporcionalmente; 1 % nele são ~1,5 °C a 90 °C.
//   `offset` — soma constante em °C, para casar com um termômetro de referência
//              no ponto de trabalho ([docs/07]).
void max_calibrar(float rref, float offset_c);
void max_get_cal(float *rref, float *offset_c);
