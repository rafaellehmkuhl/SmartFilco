#include "autotune.h"

#include <math.h>

void at_iniciar(at_t *a, float alvo, float alto, float baixo, float hist,
                int descartar, int medir, float y0)
{
    *a = (at_t){ .alvo = alvo, .alto = alto, .baixo = baixo, .hist = hist,
                 .descartar = descartar, .medir = medir,
                 .ligado = y0 < alvo, .y_max = y0, .y_min = y0 };
}

float at_passo(at_t *a, float y, float dt)
{
    a->t += dt;
    if (y > a->y_max) a->y_max = y;
    if (y < a->y_min) a->y_min = y;

    // Histerese nos dois lados: sem ela o ruído da medida faz a saída trepidar
    // no cruzamento e o "período" medido vira o do ruído.
    if (a->ligado && y > a->alvo + a->hist) {
        a->ligado = false;
    } else if (!a->ligado && y < a->alvo - a->hist) {
        a->ligado = true;
        // Um ciclo vai de subida a subida. A primeira subida só abre a contagem.
        if (a->subidas > a->descartar) {
            a->soma_tu += a->t - a->t_subida;
            a->soma_a += (a->y_max - a->y_min) / 2.0f;
            a->medidos++;
        }
        a->subidas++;
        a->t_subida = a->t;
        a->y_max = a->y_min = y;
    }
    return a->ligado ? a->alto : a->baixo;
}

bool at_pronto(const at_t *a) { return a->medidos >= a->medir; }

void at_resultado(const at_t *a, float *ku, float *tu)
{
    float amp = a->soma_a / a->medidos;
    float d = (a->alto - a->baixo) / 2.0f;
    // Com histerese ε a função descritiva do relé fica 4d/(π·√(a²−ε²)). Sem a
    // correção o Ku sai baixo, e o PID, mole.
    float r = amp * amp - a->hist * a->hist;
    *ku = 4.0f * d / ((float)M_PI * sqrtf(r > 1e-9f ? r : 1e-9f));
    *tu = a->soma_tu / a->medidos;
}

void at_ganhos_pid(float ku, float tu, float *kp, float *ki, float *kd)
{
    float kc = ku / 2.2f, ti = 2.2f * tu, td = tu / 6.3f;
    *kp = kc; *ki = kc / ti; *kd = kc * td;
}

void at_ganhos_pi(float ku, float tu, float *kp, float *ki, float *kd)
{
    float kc = ku / 3.2f, ti = 2.2f * tu;
    *kp = kc; *ki = kc / ti; *kd = 0.0f;
}
