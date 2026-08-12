// HC-SR04 — nível do reservatório, com amostragem e mediana.
//
// ⚠️ **O `ECHO` é saída de 5 V.** Ligado direto num GPIO do ESP32-S3 ele mata o
// pino. Precisa de divisor resistivo ou do level shifter que já existe para o
// ADS1115 ([docs/02]). Este código não tem como detectar se a proteção está lá.
//
// ## Quão rápido dá para ler — e por que não adianta acelerar
//
// O eco em si é rápido: a 6 cm ele volta em ~350 µs. O que limita não é o tempo
// de voo, é o **tempo de a sala calar**. Cada disparo é um trem de 8 pulsos de
// 40 kHz que continua ricocheteando depois de o eco direto chegar; se o próximo
// disparo sair antes disso assentar, o sensor mede um reflexo velho e devolve um
// número plausível e errado. O datasheet pede ciclo de **60 ms** por isso.
//
// **Reservatório é o pior caso**: estreito, alto e de parede dura, ou seja, uma
// caixa de reverberação. Por isso aqui o período é 50 ms (20 Hz) e não menos —
// e acelerar não compraria nada, porque nível de água não muda em 50 ms.
//
// ## Por que MEDIANA e não média
//
// O ruído do ultrassom **não é gaussiano**. Ele é feito de outliers: eco perdido
// vira um valor enorme, multipath vira um valor errado específico. Média de 32
// amostras com um único 3 m no meio sai deslocada 9 cm; mediana ignora o
// intruso por completo. Média só seria melhor se o ruído fosse simétrico em
// torno do valor certo, e aqui ele não é.
//
// A janela também publica **min/max e quantas passaram**, porque mediana sozinha
// esconde exatamente o que se quer investigar — foi a lição do ToF, que dava
// 73,1 cm com aparência de firmeza e 0/30 amostras válidas.

#include "ultra.h"

#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ultra";

#define PIN_TRIG 15
#define PIN_ECHO 16
#define PERIODO_MS 50          // 20 Hz; o datasheet pede >= 60 ms de ciclo, e o
                               // eco de 6 cm volta em 350 µs — sobra folga
#define ECO_TIMEOUT_US 30000
#define MIN_MM 20              // abaixo disso o HC-SR04 não garante nada
#define MAX_MM 4000
#define JANELA 32              // 32 x 50 ms = 1,6 s de histórico
#define MIN_VALIDAS 5          // com menos que isto a mediana não significa nada

static volatile int64_t t_subida;
// ⚠️ 32 bits de propósito. Leitura de 64 bits NÃO é atômica no Xtensa: a ISR pode
// atualizar entre as duas metades e a task lê um valor rasgado — que aparece como
// medida inválida esporádica, com o valor ainda parecendo plausível. Em µs,
// 32 bits cobrem 71 minutos; o timeout é de 30 ms.
static volatile uint32_t largura_us;
static volatile uint32_t ecos;

static float s_temp_c = 20.0f;
static float janela[JANELA];   // NaN = amostra inválida
static uint8_t escrita;
static float s_hz;
static TaskHandle_t s_h;

static void IRAM_ATTR eco_isr(void *arg)
{
    int64_t agora = esp_timer_get_time();
    if (gpio_get_level(PIN_ECHO)) {
        t_subida = agora;
    } else if (t_subida) {
        largura_us = (uint32_t)(agora - t_subida);
        t_subida = 0;
        ecos++;
    }
}

void ultra_set_temp(float c) { s_temp_c = c; }

static void tarefa_ultra(void *arg)
{
    TickType_t ult = xTaskGetTickCount();
    uint32_t ecos_ant = 0;
    int64_t t_hz = esp_timer_get_time();
    uint32_t n_hz = 0;

    for (uint8_t i = 0; i < JANELA; i++) janela[i] = 0.0f / 0.0f;   // NaN

    while (1) {
        // Dispara: 10 µs em nível alto é o que o HC-SR04 pede para começar.
        gpio_set_level(PIN_TRIG, 1);
        esp_rom_delay_us(10);
        gpio_set_level(PIN_TRIG, 0);

        vTaskDelayUntil(&ult, pdMS_TO_TICKS(PERIODO_MS));

        // Ao acordar, o eco deste disparo já chegou — ou não chegou nunca, e aí
        // a amostra entra como inválida em vez de repetir a anterior. Repetir
        // seria transformar sensor mudo em leitura estável.
        float mm = 0.0f / 0.0f;
        if (ecos != ecos_ant) {
            ecos_ant = ecos;
            n_hz++;
            uint32_t us = largura_us;
            if (us > 0 && us <= ECO_TIMEOUT_US) {
                // Velocidade do som varia ~0,6 m/s por °C. O BME680 já mede o
                // ambiente, então corrigir custa uma linha e vale ~1% — sobre
                // 25 cm são 2,5 mm, da ordem do que se quer resolver aqui.
                float v = (331.3f + 0.606f * s_temp_c) / 1000.0f;  // mm/µs
                float d = (float)us * v / 2.0f;
                if (d >= MIN_MM && d <= MAX_MM) mm = d;
            }
        }
        janela[escrita] = mm;
        escrita = (uint8_t)((escrita + 1) % JANELA);

        int64_t agora = esp_timer_get_time();
        if (agora - t_hz >= 1000000) {
            s_hz = (float)n_hz * 1000000.0f / (float)(agora - t_hz);
            n_hz = 0;
            t_hz = agora;
        }
    }
}

esp_err_t ultra_iniciar(void)
{
    gpio_config_t t = { .pin_bit_mask = 1ULL << PIN_TRIG, .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&t));
    gpio_set_level(PIN_TRIG, 0);

    gpio_config_t e = {
        .pin_bit_mask = 1ULL << PIN_ECHO,
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&e));

    // ESP_INTR_FLAG_IRAM porque esta ISR vai conviver com a de zero-cross, que
    // roda em IRAM: ISR fora de IRAM para durante escrita em flash.
    esp_err_t r = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) return r;   // já instalado é ok
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_ECHO, eco_isr, NULL));

    // Core 0: a amostragem é cadenciada mas não é tempo-real duro, e o core 1
    // fica reservado ao PID e à futura ISR do PSM.
    xTaskCreatePinnedToCore(tarefa_ultra, "ultra", 3072, NULL, 4, &s_h, 0);

    ESP_LOGI(TAG, "hc-sr04 trig=%d eco=%d, %d Hz, janela de %d",
             PIN_TRIG, PIN_ECHO, 1000 / PERIODO_MS, JANELA);
    return ESP_OK;
}

static int cmp(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

void ultra_ler(ultra_med_t *o)
{
    float v[JANELA];
    uint8_t n = 0;
    for (uint8_t i = 0; i < JANELA; i++) {
        float x = janela[i];
        if (x == x) v[n++] = x;      // NaN != NaN: descarta inválidas
    }

    o->total = JANELA;
    o->validas = n;
    o->ecos = ecos;
    o->hz = s_hz;

    if (n < MIN_VALIDAS) {
        o->mm = 0.0f;
        o->min_mm = o->max_mm = 0.0f;
        o->valida = false;
        return;
    }

    qsort(v, n, sizeof(float), cmp);
    o->mm = (n & 1) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0f;
    o->min_mm = v[0];
    o->max_mm = v[n - 1];
    o->valida = true;
}

// Mesma razão do ADS: durante o OTA a ISR de eco e o disparo periódico saem do
// caminho. Aqui pesa também a ISR em IRAM, que roda mesmo com o cache desligado.
void ultra_pausar(bool p)
{
    if (!s_h) return;
    if (p) vTaskSuspend(s_h); else vTaskResume(s_h);
}
