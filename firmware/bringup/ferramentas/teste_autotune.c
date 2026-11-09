// Confere o autoajuste por relé (main/autotune.c) contra plantas simuladas, de
// ponta a ponta: o relé mede Ku/Tu, os ganhos saem de Tyreus–Luyben, e o PID com
// esses ganhos tem que chegar ao alvo sem passar do ponto. Testar só Ku/Tu não
// basta — um erro de unidade nos ganhos passaria e só apareceria na caldeira.
//
//   cc -O2 -I../main teste_autotune.c ../main/autotune.c -lm -o /tmp/ta && /tmp/ta
//
// Plantas: uma caldeira (ganho de 0,5 °C/s a 100%, perda para o ambiente, atraso
// de 15 s e sensor com 20 s de inércia) e uma pressão rápida (0,3 s de atraso).
// Os números são da ordem do que a bancada mostrou, não medidas exatas — o teste
// é de que o método se adapta, não de que acerta uma planta específica.

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "autotune.h"

#define DT 0.01f
#define ATRASO_MAX 4000

typedef struct {
    float ganho, perda, amb, tau_sens;      // planta de 1ª ordem + atraso + sensor
    float x, y;
    float fila[ATRASO_MAX]; int n_atraso, i;
} planta_t;

static void planta_iniciar(planta_t *p, float ganho, float perda, float amb,
                           float atraso_s, float tau_sens, float x0)
{
    *p = (planta_t){ .ganho = ganho, .perda = perda, .amb = amb, .tau_sens = tau_sens,
                     .x = x0, .y = x0, .n_atraso = (int)(atraso_s / DT) };
}

static float planta_passo(planta_t *p, float u)
{
    p->fila[p->i] = u;
    float u_atr = p->fila[(p->i + 1) % p->n_atraso];
    p->i = (p->i + 1) % p->n_atraso;
    p->x += (p->ganho * u_atr - p->perda * (p->x - p->amb)) * DT;
    p->y += (p->x - p->y) * DT / p->tau_sens;
    return p->y + 0.02f * ((float)rand() / (float)RAND_MAX - 0.5f);   // ruído de sensor
}

// Mesmo PID de maquina.c: derivada da medida, FILTRADA (tau_d), integração
// condicional. Sem o filtro o ruído de 0,02 °C vira 2 °C/s de derivada a 100 Hz.
static float pid(float kp, float ki, float kd, float tau_d, float *i, float *m_ant,
                 float *d, float alvo, float y)
{
    float e = alvo - y;
    *d += ((y - *m_ant) / DT - *d) * DT / (DT + tau_d);
    *m_ant = y;
    float u = kp * e + ki * *i - kd * *d;
    if (!((u >= 1 && e > 0) || (u <= 0 && e < 0))) { *i += e * DT; u = kp * e + ki * *i - kd * *d; }
    return u < 0 ? 0 : u > 1 ? 1 : u;
}

static int falhas;
#define CHECA(c, ...) do { if (!(c)) { printf("✗ " __VA_ARGS__); printf("\n"); falhas++; } } while (0)

static void caso(const char *nome, planta_t pl, float alvo, float alto, float hist,
                 float t_max, float x0, int pi, float tau_d, float tol_sobre, float t_assentar)
{
    planta_t p = pl;
    at_t a;
    float y = p.y;
    at_iniciar(&a, alvo, alto, 0.0f, hist, 1, 2, y);
    float t = 0;
    while (!at_pronto(&a) && t < t_max) { y = planta_passo(&p, at_passo(&a, y, DT)); t += DT; }
    CHECA(at_pronto(&a), "%s: relé não fechou 3 ciclos em %.0f s", nome, t_max);
    if (!at_pronto(&a)) return;

    float ku, tu, kp, ki, kd;
    at_resultado(&a, &ku, &tu);
    (pi ? at_ganhos_pi : at_ganhos_pid)(ku, tu, &kp, &ki, &kd);

    // Fecha a malha a partir do frio, com os ganhos medidos.
    p = pl; planta_iniciar(&p, pl.ganho, pl.perda, pl.amb, pl.n_atraso * DT, pl.tau_sens, x0);
    float i = 0, m = p.y, d = 0, pico = -1e9, pior_final = 0; y = p.y;
    for (float tt = 0; tt < 3 * t_assentar; tt += DT) {
        y = planta_passo(&p, pid(kp, ki, kd, tau_d, &i, &m, &d, alvo, y));
        if (y > pico) pico = y;
        if (tt > t_assentar && fabsf(y - alvo) > pior_final) pior_final = fabsf(y - alvo);
    }
    printf("  %-9s Ku=%.3f Tu=%.1fs → kp=%.4f ki=%.5f kd=%.4f | passou do alvo %.2f, erro final %.2f\n",
           nome, ku, tu, kp, ki, kd, pico - alvo, pior_final);
    CHECA(pico - alvo < tol_sobre, "%s: passou %.2f do alvo (limite %.2f)", nome, pico - alvo, tol_sobre);
    CHECA(pior_final < tol_sobre / 2, "%s: não assentou, erro %.2f", nome, pior_final);
}

int main(void)
{
    srand(1);
    planta_t caldeira, pressao;
    // 0,5 °C/s a 100%; perda que pede ~12% para segurar 93 °C; atraso 15 s; sensor 20 s
    planta_iniciar(&caldeira, 0.5f, 0.5f * 0.12f / 68.0f, 25.0f, 15.0f, 20.0f, 60.0f);
    caso("caldeira", caldeira, 93.0f, 0.5f, 0.2f, 3600, 25.0f, 0, 2.0f, 1.5f, 1200);
    // pressão: 12 bar/s a 100%, escoa pelo puck, 0,3 s de atraso, sensor rápido
    planta_iniciar(&pressao, 12.0f, 1.2f, 0.0f, 0.3f, 0.05f, 0.0f);
    caso("pressão", pressao, 6.0f, 0.8f, 0.3f, 60, 0.0f, 1, 0.05f, 1.0f, 15);

    // Ku pela fórmula 4d/(πa), e o sentido da correção de histerese: com ε maior,
    // a mesma amplitude medida significa uma planta de ganho crítico MAIOR.
    at_t a = { .alto = 1, .baixo = 0, .hist = 0.0f, .soma_a = 1.0f, .soma_tu = 10, .medidos = 1 };
    float ku0, ku1, tu;
    at_resultado(&a, &ku0, &tu);
    a.hist = 0.5f;
    at_resultado(&a, &ku1, &tu);
    CHECA(ku1 > ku0, "correção de histerese invertida");
    CHECA(fabsf(ku0 - 2.0f / (float)M_PI) < 1e-4f, "Ku sem histerese = 4d/(πa) errado: %.4f", ku0);

    if (falhas) return 1;
    printf("✓ relé mede Ku/Tu e o PID resultante chega ao alvo sem passar do ponto, nas duas plantas\n");
    printf("✓ fórmula de Ku e correção de histerese conferem\n");
    return 0;
}
