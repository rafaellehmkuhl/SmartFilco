// Lógica da máquina: PID de temperatura, perfil de pressão, máquina de estados do
// shot e proteções. Fala com o mundo só pelo `hal.h` — não sabe se está no
// hardware ou na simulação, e é isso que faz o teste valer.

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MQ_OCIOSO = 0,
    MQ_AQUECENDO,
    MQ_PRONTA,
    MQ_PRE_INFUSAO,
    MQ_RAMPA,
    MQ_PLATO,
    MQ_DECLINIO,
    MQ_FIM,
    MQ_FALHA,
} mq_estado_t;

typedef struct {
    // perfil de extração
    float setpoint_c;      // temperatura alvo do boiler
    float pre_bar;         // teto de pressão na pré-infusão
    float pre_s;           // duração máxima da pré-infusão
    float plato_bar;       // pressão de extração
    float rampa_s;         // tempo para subir da pré-infusão ao platô
    float declinio_bar;    // pressão no fim
    float alvo_g;          // parada por peso — o critério principal
    float tempo_max_s;     // parada por tempo — só como rede de segurança
} mq_perfil_t;

typedef struct {
    mq_estado_t estado;
    float t_boiler;
    float pressao;
    float massa;
    float vazao;           // g/s, derivada da massa
    float nivel_pct;
    float alvo_pressao;    // o que o perfil está pedindo agora
    float ssr;
    float bomba;
    float shot_s;
    const char *falha;     // NULL = sem falha
    bool  pronta;
    float ult_g, ult_t;    // resumo do último shot; 0 = nenhum desde o boot
    // Por que o shot acabou. Painel de máquina que só diz "fim" obriga a
    // adivinhar se bateu o alvo ou estourou o tempo — e são coisas opostas.
    const char *motivo;
} mq_estado_pub_t;

void mq_iniciar(void);
void mq_passo(float dt_s);          // chamar em cadência fixa
void mq_publicar(mq_estado_pub_t *o);

void mq_shot_iniciar(void);
void mq_shot_parar(void);
void mq_perfil_get(mq_perfil_t *p);
void mq_perfil_set(const mq_perfil_t *p);
void mq_limpar_falha(void);
