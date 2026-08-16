// HAL — a costura entre a lógica da máquina e o mundo.
//
// Existe por um motivo prático: o hardware chega 05/08, e até lá a lógica de
// controle, a máquina de estados do shot, as proteções e a UI precisam poder ser
// escritas e TESTADAS. Com esta camada, trocar simulação por hardware é uma linha.
//
// A regra que mantém isso honesto: **a lógica de máquina não sabe qual
// implementação está embaixo.** Se algum dia `maquina.c` precisar perguntar
// "estou simulando?", a costura vazou e o teste deixou de valer.
//
// A docs/05 já previa esta separação ("Control Board" x "UI Board"); isto é a
// mesma disciplina aplicada um nível abaixo, entre lógica e periférico.

#pragma once

#include <stdbool.h>
#include <stdint.h>

// ---------------------------------------------------------------- entradas

typedef struct {
    float t_boiler;    // °C na parede do boiler (PT100 + MAX31865)
    float pressao;     // bar no grupo (sensor 200 PSI via ADS1115)
    float massa;       // g na xícara (célula de carga)
    float nivel_mm;    // distância sensor→água no reservatório (HC-SR04)

    // Falhas vindas do hardware, não inferidas. O MAX31865 tem detecção por
    // registrador; usá-la é melhor que heurística sobre o valor.
    bool falha_temp;   // sonda aberta ou em curto
    bool falha_press;  // fora da faixa elétrica (0,5–4,5 V)
    bool falha_massa;  // amplificador não responde
} hal_ent_t;

// ---------------------------------------------------------------- saídas

typedef struct {
    float ssr;     // 0..1 — fração da janela lenta de PWM na resistência
    float bomba;   // 0..1 — duty do PSM (fração de semiciclos disparados)
} hal_sai_t;

// ---------------------------------------------------------------- interface

// O barramento I²C é criado no main (BME680 e RTC já o usam); o HAL recebe o
// handle em vez de criar o próprio, senão seriam dois drivers disputando os
// mesmos pinos. Chamar ANTES de hal_iniciar.
void hal_set_bus(void *bus);

// Põe os pinos dos atuadores em nível baixo. Chamar como PRIMEIRA coisa do
// app_main — ver o comentário em hal_real.c sobre a janela que ela não cobre.
void hal_atuadores_seguros(void);

void hal_iniciar(void);
void hal_ler(hal_ent_t *e);
void hal_escrever(const hal_sai_t *s);

// `dt_s` é passado de fora para a simulação poder rodar acelerada sem que a
// lógica de controle saiba. Na implementação real ele é ignorado.
void hal_passo(float dt_s);

// ------------------------------------------------------- troca de implementação

typedef enum { HAL_SIM = 0, HAL_REAL = 1 } hal_modo_t;

void hal_set_modo(hal_modo_t m);
hal_modo_t hal_modo(void);

// --------------------------------------------------- controles da simulação
//
// Só a UI de simulação toca aqui. A lógica de máquina nunca inclui isto.

typedef struct {
    float t_ambiente;      // °C
    float pot_resistencia; // W — a da Philco, a confirmar ao abrir ([docs/06])
    float moagem;          // resistência do puck, 1,0 = referência
    float nivel_inicial;   // mm
    float vel;             // aceleração de tempo, 1 = tempo real
    bool  sem_agua;
    bool  falha_temp;      // injeção de falha
    bool  falha_press;
    bool  puck_ausente;    // sem porta-filtro: vazão livre, pressão não sobe
} sim_cfg_t;

void sim_cfg_get(sim_cfg_t *c);
void sim_cfg_set(const sim_cfg_t *c);
void sim_reset(void);

// Estado interno visível só para a UI de simulação — mostra o que o modelo
// "sabe" e a máquina não mede, que é justamente o que ajuda a julgar a UI.
typedef struct {
    float vazao_g_s;
    float t_agua_grupo;    // temperatura da água que chega ao bolo
    float energia_w;       // potência instantânea na resistência
} sim_espia_t;

void sim_espiar(sim_espia_t *o);
