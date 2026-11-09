// Autoajuste por relé (Åström–Hägglund). Lógica pura, sem hardware: recebe a
// medida, devolve a saída, e no fim entrega o ganho e o período críticos (Ku, Tu).
// Serve às duas malhas — caldeira e pressão — porque o método não sabe o que está
// controlando, só que a saída liga acima do alvo e desliga abaixo.
//
// Por que relé e não degrau: o relé mede a planta NO PONTO DE TRABALHO, com a
// perda de calor e a não-linearidade da bomba que existem ali, e se estabiliza
// sozinho em torno do alvo. O degrau mede de onde partiu e extrapola.

#pragma once

#include <stdbool.h>

typedef struct {
    float alvo, alto, baixo, hist;
    int descartar, medir;      // ciclos ignorados no começo / ciclos que contam
    bool ligado;
    int subidas;               // quantas vezes a saída foi para `alto`
    float t, t_subida, y_max, y_min;
    float soma_tu, soma_a;
    int medidos;
} at_t;

void at_iniciar(at_t *a, float alvo, float alto, float baixo, float hist,
                int descartar, int medir, float y0);
// Avança um passo e devolve a saída a aplicar.
float at_passo(at_t *a, float y, float dt);
bool at_pronto(const at_t *a);
// Ku e Tu dos ciclos medidos. Só vale com at_pronto().
void at_resultado(const at_t *a, float *ku, float *tu);
// Tyreus–Luyben: menos agressivo que Ziegler–Nichols, que dá ~25% de sobressinal.
// Caldeira passando do ponto é café queimado; pressão passando é puck rasgado.
// Ganhos na forma do PID da máquina: u = kp·e + ki·∫e + kd·(−dy/dt).
void at_ganhos_pid(float ku, float tu, float *kp, float *ki, float *kd);
void at_ganhos_pi(float ku, float tu, float *kp, float *ki, float *kd);
