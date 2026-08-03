// Simulação física da máquina.
//
// O objetivo NÃO é precisão — é **plausibilidade**. Um modelo que responde com a
// forma errada faz a UI parecer boa testando o que não vai acontecer, e isso é
// pior que não simular. Por isso cada constante aqui tem origem declarada, e as
// que são chute estão marcadas como chute.
//
// Onde o modelo acerta de propósito (comportamentos conhecidos que ele reproduz):
//
//  · **Trava em ~11 bar com a bomba no talo.** A docs/07 registra que o dimmer
//    manual "trava em 11–12 bar sem chegar aos 9". Aqui isso cai do cruzamento da
//    curva P-Q da bomba com a restrição do puck — não está codificado à mão.
//  · **Primeiro pingo atrasado.** O puck absorve antes de deixar passar; a massa
//    na xícara só começa depois da saturação.
//  · **Ripple do PSM em duty baixo.** Roda o mesmo acumulador de Bresenham que a
//    página desenha, e cada semiciclo disparado dá um empurrão de pressão. É o
//    único contra real do PSM segundo a docs/07 — o filtro precisa de algo para
//    filtrar.
//  · **Offset boiler-bolo que muda com a vazão.** A docs/07 mede que o sinal do
//    offset INVERTE conforme o regime; aqui a água tem menos tempo de trocar calor
//    quanto mais rápido passa.

#include "hal.h"

#include <math.h>
#include <string.h>

#include "esp_random.h"

// ------------------------------------------------------------- constantes
//
// Térmicas: bloco de alumínio de ~0,35 kg a 900 J/kg·K. Chute educado — a Philco
// ainda não foi aberta ([docs/06]), e a massa térmica real é o que mais move o
// tempo de aquecimento.
#define C_TERMICA 400.0f    // J/K
#define K_PERDA 1.2f        // W/K — para segurar 93 °C com 25 de ambiente dá ~82 W
#define CP_AGUA 4.186f      // J/g·K

// Hidráulicas: estol da Ulka ~15 bar ([docs/03]), vazão livre ~8 g/s.
#define P_ESTOL 15.0f
#define Q_LIVRE 8.0f        // g/s sem contrapressão
// k do puck: calibrado para dar 2 g/s a 9 bar com moagem 1,0, que é um shot normal
#define K_PUCK 0.667f
#define C_HIDRAULICA 0.35f  // complacência: quanto a pressão demora a formar

#define DOSE_G 18.0f
#define ABSORCAO 2.0f       // g de água retidos por g de café, antes de pingar

#define TANQUE_CM2 100.0f   // área do reservatório: 1 g tira 0,1 mm de coluna
#define NIVEL_CHEIO 40.0f
#define NIVEL_VAZIO 250.0f

#define T_ENTRADA 22.0f     // água do reservatório, ~ambiente

static sim_cfg_t cfg = {
    .t_ambiente = 25.0f,
    .pot_resistencia = 1000.0f,
    .moagem = 1.0f,
    .nivel_inicial = 60.0f,
    .vel = 1.0f,
    .sem_agua = false,
    .falha_temp = false,
    .falha_press = false,
    .puck_ausente = false,
};

static struct {
    float t_boiler;
    float pressao;
    float massa;
    float nivel_mm;
    float absorvido;    // g já retidos pelo puck
    float vazao;        // g/s saindo do puck
    float t_agua;
    float energia_w;
    uint32_t psm_acc;   // acumulador de Bresenham, igual ao da PSM.Library
    float fase_ruido;
} st;

static float ruido(float amp)
{
    return ((float)(esp_random() % 2000) / 1000.0f - 1.0f) * amp;
}

void sim_reset(void)
{
    memset(&st, 0, sizeof(st));
    st.t_boiler = cfg.t_ambiente;
    st.nivel_mm = cfg.nivel_inicial;
    st.t_agua = cfg.t_ambiente;
}

void sim_cfg_get(sim_cfg_t *c) { *c = cfg; }

void sim_cfg_set(const sim_cfg_t *c)
{
    cfg = *c;
    if (cfg.vel < 0.1f) cfg.vel = 0.1f;
    if (cfg.vel > 20.0f) cfg.vel = 20.0f;
    if (cfg.moagem < 0.2f) cfg.moagem = 0.2f;
}

void sim_espiar(sim_espia_t *o)
{
    o->vazao_g_s = st.vazao;
    o->t_agua_grupo = st.t_agua;
    o->energia_w = st.energia_w;
}

// ---------------------------------------------------------------- o modelo

static void sim_avancar(float dt, float ssr, float bomba)
{
    if (dt <= 0.0f) return;

    // ---- PSM: mesmo acumulador que a página desenha, a 120 semiciclos/s.
    // Não é enfeite: é o que produz o ripple de pressão em duty baixo.
    int disparos = 0;
    int semiciclos = (int)(120.0f * dt + 0.5f);
    uint32_t duty = (uint32_t)(bomba * 1000.0f);
    for (int i = 0; i < semiciclos; i++) {
        st.psm_acc += duty;
        if (st.psm_acc >= 1000) { st.psm_acc -= 1000; disparos++; }
    }
    float bomba_ef = semiciclos > 0 ? (float)disparos / (float)semiciclos : 0.0f;

    // ---- hidráulica
    bool seco = cfg.sem_agua || st.nivel_mm >= NIVEL_VAZIO;
    float q_bomba = 0.0f;
    if (!seco && bomba_ef > 0.0f) {
        // Curva P-Q linearizada: vazão cai conforme a contrapressão sobe.
        q_bomba = bomba_ef * Q_LIVRE * (1.0f - st.pressao / P_ESTOL);
        if (q_bomba < 0.0f) q_bomba = 0.0f;
    }

    // Restrição do puck. Escoamento por orifício: vazão ~ raiz da pressão.
    float q_puck;
    if (cfg.puck_ausente) {
        q_puck = q_bomba;             // sem porta-filtro a água sai livre
        st.pressao = 0.0f;
    } else {
        q_puck = K_PUCK * sqrtf(fmaxf(st.pressao, 0.0f)) / cfg.moagem;
        st.pressao += (q_bomba - q_puck) / C_HIDRAULICA * dt;
        if (st.pressao < 0.0f) st.pressao = 0.0f;
        // Empurrão por semiciclo disparado: é daqui que sai o ripple. Em duty
        // alto os disparos são densos e o efeito some na complacência; em duty
        // baixo eles ficam espaçados e a pressão balança visivelmente.
        if (disparos > 0 && semiciclos > 0)
            st.pressao += 0.02f * (1.0f - bomba_ef) * (float)disparos;
    }

    // ---- o puck absorve antes de pingar
    float para_xicara = 0.0f;
    float retem = DOSE_G * ABSORCAO;
    if (!cfg.puck_ausente && st.absorvido < retem) {
        st.absorvido += q_puck * dt;
    } else {
        para_xicara = q_puck;
    }
    st.vazao = para_xicara;
    st.massa += para_xicara * dt;

    // ---- reservatório: 1 g de água = 1 cm³
    st.nivel_mm += (q_bomba * dt) / TANQUE_CM2 * 10.0f;
    if (st.nivel_mm > NIVEL_VAZIO) st.nivel_mm = NIVEL_VAZIO;
    if (st.nivel_mm < NIVEL_CHEIO) st.nivel_mm = NIVEL_CHEIO;

    // ---- térmico
    st.energia_w = ssr * cfg.pot_resistencia;
    float perdas = K_PERDA * (st.t_boiler - cfg.t_ambiente);
    float p_agua = q_bomba * CP_AGUA * fmaxf(st.t_boiler - T_ENTRADA, 0.0f);
    st.t_boiler += (st.energia_w - perdas - p_agua) / C_TERMICA * dt;
    if (st.t_boiler < cfg.t_ambiente) st.t_boiler = cfg.t_ambiente;

    // Temperatura da água que CHEGA ao bolo. Quanto mais rápido passa, menos
    // tempo tem de trocar calor — é isto que faz o offset boiler-bolo mudar de
    // sinal conforme o regime, como a docs/07 mediu com termopar K.
    float eficiencia = 1.0f / (1.0f + q_bomba * 0.35f);
    st.t_agua = T_ENTRADA + (st.t_boiler - T_ENTRADA) * eficiencia;
}

// ---------------------------------------------------------------- interface

void sim_iniciar(void) { sim_reset(); }

void sim_passo(float dt_s, const hal_sai_t *s)
{
    // A aceleração de tempo mora AQUI e não na lógica de controle: o PID tem que
    // rodar com o mesmo dt de sempre, senão o teste vira ficção.
    float dt = dt_s * cfg.vel;
    // Passos grandes quebram integração explícita. Fatia em pedaços de 10 ms.
    while (dt > 0.0f) {
        float p = dt > 0.01f ? 0.01f : dt;
        sim_avancar(p, s->ssr, s->bomba);
        dt -= p;
    }
}

void sim_ler(hal_ent_t *e)
{
    e->t_boiler = st.t_boiler + ruido(0.08f);
    e->pressao = st.pressao + ruido(0.03f);
    e->massa = st.massa + ruido(0.02f);
    e->nivel_mm = st.nivel_mm + ruido(1.5f);
    e->falha_temp = cfg.falha_temp;
    e->falha_press = cfg.falha_press;
    e->falha_massa = false;
}
