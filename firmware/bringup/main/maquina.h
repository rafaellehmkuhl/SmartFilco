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
    float teste_s;         // segundos restantes do teste de bancada; 0 = parado
    bool  teste_ssr;       // o teste é (ou foi) do SSR; false = da bomba
    // autoajuste: rodando, de qual malha, subidas do relé até agora / necessárias,
    // e por que o último acabou ("ok", "manual", "maq", "tempo", código de falha...)
    bool  cal_on;
    int   cal_malha;
    int   cal_passo, cal_total;
    const char *cal_fim;
    const char *teste_fim; // por que o último teste acabou: "tempo", "manual", ...
} mq_estado_pub_t;

void mq_iniciar(void);
void mq_passo(float dt_s);          // chamar em cadência fixa
void mq_publicar(mq_estado_pub_t *o);

// A máquina NASCE em repouso, com a resistência desligada. Sair de lá é ato
// explícito — ver o comentário em maquina.c sobre o estado seguro no boot.
void mq_ligar(bool on);
bool mq_ligada(void);

void mq_shot_iniciar(void);
void mq_shot_parar(void);
void mq_perfil_get(mq_perfil_t *p);
void mq_perfil_set(const mq_perfil_t *p);
void mq_limpar_falha(void);

// Teste de bancada da bomba: duty fixo por tempo limitado, FORA de shot.
// Ignora temperatura e nível de água de propósito (debug com máquina fria ou
// seca). Continua valendo: teto de MQ_TESTE_MAX_S, corte por sobrepressão e
// qualquer FALHA. Recusado durante shot e em falha.
// O do SSR só com a máquina desligada, até 60 s, e acaba ao chegar no setpoint.
#define MQ_TESTE_MAX_S 10.0f
#define MQ_TESTE_SSR_MAX_S 60.0f
void mq_teste(bool ssr, float duty, float s);
void mq_teste_parar(void);

// ---- ganhos dos PIDs e autoajuste por relé (autotune.c)
//
// Os ganhos vivem na NVS: sobrevivem a reboot e a OTA. `origem` diz de onde vieram
// — quem olha um kp precisa saber se foi medido nesta caldeira ou digitado.
typedef enum { MQ_MALHA_TEMP = 0, MQ_MALHA_PRESS, MQ_MALHAS } mq_malha_t;
typedef enum { MQ_GANHO_PADRAO = 0, MQ_GANHO_CALIBRADO, MQ_GANHO_MANUAL } mq_origem_t;
typedef struct {
    float kp, ki, kd;
    float ku, tu;          // da última calibração; 0 = nunca calibrado
    uint8_t origem;        // mq_origem_t
} mq_ganhos_t;
void mq_ganhos_get(mq_malha_t m, mq_ganhos_t *g);
void mq_ganhos_set(mq_malha_t m, float kp, float ki, float kd);   // limita e salva
void mq_ganhos_padrao(mq_malha_t m);
// Só com a máquina DESLIGADA: a calibração é dona do atuador enquanto roda.
// Temperatura: relé em torno do setpoint, 10 a 20 min. Pressão: relé em 6 bar,
// ~20 s, com porta-filtro e café (cesta cega não escoa — não oscila).
void mq_cal_iniciar(mq_malha_t m);
// Ignora o sensor do reservatório (nível não barra shot nem calibração). Salvo.
void mq_ignora_nivel(bool on);
bool mq_ignorando_nivel(void);
void mq_cal_parar(void);
