// SmartFilco — bring-up em ESP-IDF
// RGB WS2812 em passeio semi-aleatório + página web mostrando a cor ao vivo.
//
// Divisão de cores segue a docs/05: core 1 = tempo-real (aqui, a animação do
// LED, que depois vira o laço de controle), core 0 = Wi-Fi/HTTP.

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "driver/i2c_master.h"
#include "bme680.h"
#include "esp_netif_sntp.h"
#include "hal.h"
#include "maquina.h"
#include "rtc.h"
#include "sd.h"
#include "ultra.h"
#include "tof.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#include "wifi_cfg.h"

static const char *TAG = "bringup";

// DevKitC-1 v1.0 usa GPIO48, v1.1 usa GPIO38 — a Espressif trocou entre
// revisões. Aciona os dois: o pino sem LED só chaveia à toa.
#define PIN_A 48
#define PIN_B 38

#define MAXV 80          // teto de brilho (V). WS2812 no máximo ofusca e puxa ~60 mA
#define SAT 255          // saturação cheia — é o que impede o cinza
#define FRAME_MS 20      // 50 fps
#define TRANSICAO_MS 3000
#define CORE_RT 1        // core do tempo-real
#define CORE_NET 0       // core da rede
// httpd_get_client_list() devolve ESP_ERR_INVALID_ARG se houver mais sockets
// abertos que o array comporta — por isso o mesmo valor governa os dois.
#define MAX_SOCKS 7      // = httpd_config_t.max_open_sockets
#define WS_HZ 30         // taxa de push do estado (33 ms)

// Barramento I²C da bancada. Mesmos pinos que a docs/02 reserva ao projeto, para
// a prototipagem não virar refação de chicote depois.
#define I2C_SDA 8
#define I2C_SCL 9
#define I2C_LO 0x08      // faixa 7-bit válida; fora disso é endereço reservado
#define I2C_HI 0x77
#define MAX_I2C 16       // achados por varredura — mais que isso não cabe no barramento na prática
#define SCAN_MS 2000
#define SENSOR_MS 100    // leitura dos sensores = 10 Hz; a varredura sai a cada 20 destas
#define ADDR_TOF 0x29
#define ADDR_RTC 0x68
// Brasil não tem mais horário de verão desde 2019, então offset fixo de -3 basta.
// Um dia com regra de DST exigiria a base tzdata inteira no flash.
#define TZ_BR "<-03>3"
#define BME_MS 1000      // BME680 em modo forçado com aquecedor bloqueia ~400 ms:
                         // task própria, senão o tanque engasgaria junto

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static led_strip_handle_t tiras[2];
static int64_t t0;
static httpd_handle_t servidor;
// Brilho corrente, 0..MAXV. Ajustável pela página via WebSocket; 0 = apagado.
// O passeio de matiz continua rodando com brilho 0 — ao reacender, a cor está no
// ponto em que estaria, em vez de saltar.
static volatile uint8_t brilho = MAXV;
static i2c_master_bus_handle_t i2c_bus;

// ponytail: escrita sem lock, lida pelo handler HTTP. A ordem importa — o array é
// preenchido ANTES de publicar a contagem, então um leitor vê sempre um par
// (n, dados) coerente. Trocar por mutex quando isto virar algo além de bancada.
static volatile uint8_t i2c_achados[MAX_I2C];
static volatile uint8_t i2c_n;

// Leituras publicadas. Inteiros de propósito: evita formatar float no firmware e
// deixa a página decidir a apresentação.
static bme680_handle_t bme;
static volatile bool bme_ok, tof_ok;
static volatile int32_t s_temp_c100;
static volatile uint32_t s_press_pa, s_umid_pct100, s_gas_ohm;
static volatile uint16_t s_dist_mm;
static volatile uint8_t s_dist_status = 0xFF;   // 0 = medida válida
// sigma e sinal saem junto porque sem eles não dá para julgar a distância:
// número sozinho não diz se o alvo encheu o cone de 27° ou se a medida é uma
// mistura de alvo + fundo.
static volatile uint16_t s_dist_sigma, s_dist_sinal, s_dist_amb;
static volatile int16_t s_offset_mm;   // calibração de offset, só em RAM
static volatile uint16_t s_dist_n;     // nº da medida; a página usa p/ não contar repetida
static volatile uint16_t s_dist_mm2;   // contraprova via GetDistance

// Ultrassônico publicado CRU, independente do modo do HAL. Em simulação o
// `real_ler()` não roda, e sem isto o sensor ficaria mudo justamente quando se
// quer comparar a leitura de verdade com a do modelo.
static volatile float s_u_mm, s_u_lo, s_u_hi, s_u_hz;
static volatile bool s_u_ok;
static volatile uint8_t s_u_val, s_u_tot;

// Relógio. `s_rtc_fonte` é o que dá crédito à hora: sem ele o painel mostraria um
// horário formatadinho vindo de um oscilador que parou em 2019.
static volatile bool rtc_ok;
static char s_hora[9] = "--:--:--";    // HH:MM:SS
static char s_data[11] = "--/--/----";
static volatile int16_t s_rtc_temp_c100;
static const char *s_rtc_fonte = "sem rtc";   // ntp | rtc | INVÁLIDA | sem rtc

static volatile int ws_falhas;         // sends recusados, acumulado
static volatile uint32_t ws_enviados;  // frames entregues, acumulado

// ponytail: 3 bytes lidos pelo handler HTTP sem lock. Uma leitura rasgada mostra
// uma cor intermediária por um quadro e nada mais — não vale um mutex. Quando o
// mesmo padrão valer para setpoint de PID, aí sim precisa de proteção.
static volatile uint8_t cor[3];

static EventGroupHandle_t eventos;
#define BIT_CONECTADO BIT0

// ---------------------------------------------------------------- animação

// Interpolação de MATIZ pelo arco curto, por TEMPO.
//
// Duas escolhas aqui, ambas para não cair no cinza:
//  - sortear matiz em vez de R/G/B independentes. Três uniformes independentes
//    se concentram na média, e o resultado é quase sempre cinza claro.
//  - interpolar o matiz, não o RGB. Interpolar RGB de vermelho a ciano passa
//    pelo cinza no meio; andar pelo círculo de matiz mantém tudo saturado.
static uint16_t interp_matiz(uint16_t a, uint16_t b, int prog, int total)
{
    int d = (int)b - (int)a;
    if (d > 180) d -= 360;      // sempre pelo lado curto do círculo
    if (d < -180) d += 360;
    return (uint16_t)(((int)a + d * prog / total + 360) % 360);
}

static void hsv_para_rgb(uint16_t h, uint8_t s, uint8_t v, volatile uint8_t *out)
{
    h %= 360;
    uint8_t regiao = h / 60;
    uint32_t resto = (h % 60) * 255 / 60;
    uint8_t p = (v * (255 - s)) / 255;
    uint8_t q = (v * (255 - (s * resto) / 255)) / 255;
    uint8_t t = (v * (255 - (s * (255 - resto)) / 255)) / 255;

    switch (regiao) {
        case 0:  out[0] = v; out[1] = t; out[2] = p; break;
        case 1:  out[0] = q; out[1] = v; out[2] = p; break;
        case 2:  out[0] = p; out[1] = v; out[2] = t; break;
        case 3:  out[0] = p; out[1] = q; out[2] = v; break;
        case 4:  out[0] = t; out[1] = p; out[2] = v; break;
        default: out[0] = v; out[1] = p; out[2] = q; break;
    }
}

static void autoteste(void)
{
    assert(interp_matiz(0, 90, 0, 100) == 0);      // começo
    assert(interp_matiz(0, 90, 100, 100) == 90);   // fim exato no alvo
    assert(interp_matiz(0, 90, 50, 100) == 45);    // meio
    assert(interp_matiz(350, 10, 50, 100) == 0);   // cruza o zero pelo arco curto
    assert(interp_matiz(10, 350, 50, 100) == 0);   // e no sentido inverso

    uint8_t c[3];
    hsv_para_rgb(0, 255, 80, c);
    assert(c[0] == 80 && c[1] == 0 && c[2] == 0);   // matiz 0 = vermelho puro
    hsv_para_rgb(120, 255, 80, c);
    assert(c[1] == 80 && c[0] == 0);                // 120 = verde
    hsv_para_rgb(240, 255, 80, c);
    assert(c[2] == 80 && c[1] == 0);                // 240 = azul
}

static uint16_t sortear_matiz(void)
{
    return esp_random() % 360;
}

static led_strip_handle_t criar_tira(int gpio)
{
    led_strip_config_t cfg = {
        .strip_gpio_num = gpio,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    led_strip_handle_t h = NULL;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&cfg, &rmt, &h));
    return h;
}

static void tarefa_led(void *arg)
{
    uint16_t de = sortear_matiz();
    uint16_t para = sortear_matiz();
    int64_t t_ini = esp_timer_get_time() / 1000;
    int quadros = 0;
    uint16_t matiz = de;

    while (1) {
        int prog = (int)(esp_timer_get_time() / 1000 - t_ini);
        if (prog >= TRANSICAO_MS) {
            de = para;
            para = sortear_matiz();
            t_ini = esp_timer_get_time() / 1000;
            prog = 0;
        }
        matiz = interp_matiz(de, para, prog, TRANSICAO_MS);
        hsv_para_rgb(matiz, SAT, brilho, cor);

        for (int t = 0; t < 2; t++) {
            ESP_ERROR_CHECK(led_strip_set_pixel(tiras[t], 0, cor[0], cor[1], cor[2]));
            ESP_ERROR_CHECK(led_strip_refresh(tiras[t]));
        }

        // Heartbeat. Com USB-Serial-JTAG não dá para capturar o boot de forma
        // confiável (o reset derruba a própria USB), então o firmware precisa
        // falar sozinho — senão a placa fica indepurável sem monitor interativo.
        if (++quadros % (2000 / FRAME_MS) == 0) {
            ESP_LOGI(TAG, "matiz=%u  brilho=%u  rgb=%u,%u,%u  heap=%lu  core=%d",
                     matiz, brilho, cor[0], cor[1], cor[2],
                     (unsigned long)esp_get_free_heap_size(), xPortGetCoreID());
        }

        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
}

// ---------------------------------------------------------------- i²c

static void i2c_iniciar(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA,
        .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        // Os módulos já trazem pull-up de 4,7 k; o interno (~45 k) só entra em
        // paralelo e não atrapalha. Fica ligado como rede de segurança para
        // testar um dispositivo que venha sem pull-up próprio.
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &i2c_bus));
}

// Task própria porque `bme680_get_data` faz a medida em modo forçado — liga o
// aquecedor e espera. Junto com o ToF, esse bloqueio viraria engasgo no tanque.
static void tarefa_bme(void *arg)
{
    bme680_config_t cfg = I2C_BME680_CONFIG_DEFAULT;   // já vem em 0x77, que é o medido
    bme_ok = (bme680_init(i2c_bus, &cfg, &bme) == ESP_OK && bme != NULL);
    ESP_LOGI(TAG, "bme680: %s", bme_ok ? "ok" : "AUSENTE");

    while (1) {
        if (bme_ok) {
            bme680_data_t d;
            if (bme680_get_data(bme, &d) == ESP_OK) {
                s_temp_c100 = (int32_t)(d.air_temperature * 100.0f);
                s_umid_pct100 = (uint32_t)(d.relative_humidity * 100.0f);
                // ⚠️ O header do componente documenta este campo como hPa. É PASCAL.
                // Multiplicar por 100 confiando no comentário dá 100x a pressão real
                // (954 hPa vira 95423) — e passa despercebido se ninguém souber o
                // valor esperado. Aqui, ~954 hPa bate com os ~600 m de São José.
                s_press_pa = (uint32_t)d.barometric_pressure;
                s_gas_ohm = (uint32_t)d.gas_resistance;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BME_MS));
    }
}

// ------------------------------------------------------------------- relógio
//
// Os dois sentidos, e é isso que a bancada existe para provar:
//
//   NTP → RTC   quando há rede, o DS3231 recebe a hora certa e passa a guardá-la
//   RTC → sistema   quando não há, o relógio do ESP32 nasce com a hora do DS3231
//                   em vez de 1970
//
// A docs/05 removeu o `RTCModule` do projeto argumentando que não existe requisito
// de log correto sem rede. O argumento continua valendo; o que muda é que agora dá
// para revisitar com dado — este código mostra exatamente o que se ganha.
static void hora_sincronizar(void)
{
    struct tm t = { 0 };

    // 1) tenta a rede. 8 s: além disso o boot fica visivelmente travado, e a hora
    //    não vale segurar a subida do servidor web.
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&cfg);
    bool via_ntp = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(8000)) == ESP_OK;

    if (via_ntp) {
        s_rtc_fonte = "ntp";
        if (rtc_ok) {
            time_t agora = time(NULL);
            localtime_r(&agora, &t);
            if (rtc_gravar(&t) == ESP_OK)
                ESP_LOGI(TAG, "ds3231 acertado pelo ntp");
        }
        return;
    }

    ESP_LOGW(TAG, "ntp não respondeu");
    if (!rtc_ok) return;

    // 2) sem rede: o RTC vira a fonte — mas só se ele mesmo disser que a hora vale.
    //    OSF de pé significa que o oscilador parou; o chip devolve uma hora bem
    //    formatada e sem nenhum vínculo com a realidade.
    if (rtc_perdeu_hora()) {
        s_rtc_fonte = "INVÁLIDA";
        ESP_LOGE(TAG, "ds3231 com OSF de pé — hora descartada, não adianta ler");
        return;
    }
    if (rtc_ler(&t) != ESP_OK) return;
    struct timeval tv = { .tv_sec = mktime(&t), .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_rtc_fonte = "rtc";
    ESP_LOGI(TAG, "hora veio do ds3231 (sem rede)");
}

// -------------------------------------------------------------- controle
//
// 100 Hz no core de tempo-real. A docs/05 pede 50–100 ms para o laço; 10 ms dá
// folga e é o que a integração da simulação precisa para não acumular erro.
//
// `dt` vem do relógio real, não da constante do delay: se a task atrasar, o PID
// e o modelo têm que saber. dt fixo com task atrasada é como o painel mostrar
// valor antigo achando que é novo.
#define CTRL_MS 10

static void tarefa_controle(void *arg)
{
    mq_iniciar();
    TickType_t ult = xTaskGetTickCount();
    int64_t t_ant = esp_timer_get_time();
    while (1) {
        int64_t agora = esp_timer_get_time();
        float dt = (float)(agora - t_ant) / 1000000.0f;
        t_ant = agora;
        if (dt > 0.2f) dt = 0.2f;      // pós-boot ou atraso grande: não integrar lixo
        mq_passo(dt);
        vTaskDelayUntil(&ult, pdMS_TO_TICKS(CTRL_MS));
    }
}

static void tarefa_relogio(void *arg)
{
    // A sincronização mora AQUI, não no app_main, porque ela conversa com a rede e
    // pode demorar. No app_main ela ficava na frente do http_iniciar() e um NTP
    // lento atrasava o painel inteiro — hora é enfeite, painel é a interface.
    hora_sincronizar();

    while (1) {
        if (rtc_ok) {
            struct tm t;
            if (rtc_ler(&t) == ESP_OK && t.tm_year > 100) {
                strftime(s_hora, sizeof(s_hora), "%H:%M:%S", &t);
                strftime(s_data, sizeof(s_data), "%d/%m/%Y", &t);
            }
            int16_t c = 0;
            if (rtc_temp_c100(&c) == ESP_OK) s_rtc_temp_c100 = c;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void sensores_iniciar(void)
{
    // Modo curto (~1,3 m): é o certo para reservatório (4-25 cm) e presença de
    // copo, e é mais imune a luz ambiente que o longo.
    //
    // Orçamento 50 ms, período 60 ms — de propósito MAIS RÁPIDO que os 10 Hz da
    // task. Se o sensor medisse a 100 ms igual à task, as duas cadências andariam
    // em fase deslizante e cada leitura pegaria a medida ora pronta ora não,
    // bloqueando a task para esperar. Com o sensor à frente, todo ciclo encontra
    // dado fresco e `tof_ler` volta na hora. O orçamento continua 50 ms porque foi
    // o que deu sigma baixo — encurtar isso é comprar ruído.
    rtc_ok = rtc_iniciar(i2c_bus, ADDR_RTC) == ESP_OK;
    ESP_LOGI(TAG, "ds3231: %s", rtc_ok ? "ok" : "AUSENTE");

    tof_ok = (tof_iniciar(i2c_bus, ADDR_TOF) == ESP_OK)
             && (tof_config_curta(50, 60) == ESP_OK)
             && (tof_start() == ESP_OK);
    ESP_LOGI(TAG, "vl53l1x (ULD): %s", tof_ok ? "ok" : "AUSENTE");
}

static void tarefa_i2c(void *arg)
{
    uint8_t ant[MAX_I2C] = { 0 };
    uint8_t ant_n = 0xFF;              // força o primeiro log
    int ciclo = 0;

    sensores_iniciar();

    while (1) {
        if (tof_ok) {
            tof_leitura_t r;
            // timeout curto: se a medida não estiver pronta, tenta de novo no
            // próximo ciclo em vez de segurar a task
            if (tof_ler(&r, 30) == ESP_OK) {
                s_dist_mm = r.mm;
                s_dist_mm2 = r.mm_direto;
                s_dist_status = r.status;
                s_dist_sigma = r.sigma_mm;
                s_dist_sinal = r.sinal_kcps;
                s_dist_amb = r.ambiente_kcps;
                // Contador de medida. O WebSocket empurra a 30 Hz e o sensor mede
                // a 10 — sem isto a página conta a mesma leitura três vezes, o que
                // infla "30/30 válidas" e estreita a faixa. Diagnóstico que se
                // auto-engana é pior que diagnóstico nenhum.
                s_dist_n++;
            }
        }

        // HC-SR04: lê o eco do disparo anterior e dispara de novo. Separados
        // porque o eco leva até 25 ms e esta task roda a cada 100 ms.
        ultra_med_t um;
        ultra_ler(&um);
        s_u_mm = um.mm; s_u_lo = um.min_mm; s_u_hi = um.max_mm;
        s_u_ok = um.valida; s_u_val = um.validas; s_u_tot = um.total;
        s_u_hz = um.hz;
        ultra_set_temp((float)s_temp_c100 / 100.0f);   // corrige a velocidade do som

        if (ciclo++ % (SCAN_MS / SENSOR_MS) != 0) {
            vTaskDelay(pdMS_TO_TICKS(SENSOR_MS));
            continue;
        }

        uint8_t achou[MAX_I2C];
        uint8_t n = 0;

        for (uint16_t a = I2C_LO; a <= I2C_HI && n < MAX_I2C; a++) {
            if (i2c_master_probe(i2c_bus, a, 20) == ESP_OK) {
                achou[n++] = (uint8_t)a;
            }
        }

        for (uint8_t i = 0; i < n; i++) {
            i2c_achados[i] = achou[i];
        }
        i2c_n = n;                     // publica a contagem só depois dos dados

        // Loga só quando MUDA. Na bancada isso vira "apareceu / sumiu" ao plugar,
        // em vez de encher a serial com a mesma lista a cada 2 s.
        if (n != ant_n || memcmp(ant, achou, n) != 0) {
            char lista[MAX_I2C * 6 + 1] = { 0 };
            int p = 0;
            for (uint8_t i = 0; i < n; i++) {
                p += snprintf(lista + p, sizeof(lista) - p, "0x%02x ", achou[i]);
            }
            ESP_LOGI(TAG, "i2c: %d dispositivo(s)  %s", n, n ? lista : "(nenhum)");
            memcpy(ant, achou, n);
            ant_n = n;
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_MS));
    }
}

// ---------------------------------------------------------------- wi-fi

static void ao_evento(void *arg, esp_event_base_t base, int32_t id, void *dados)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(eventos, BIT_CONECTADO);
        ESP_LOGW(TAG, "link caiu, reconectando");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)dados;
        ESP_LOGI(TAG, "conectado: ip=" IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(eventos, BIT_CONECTADO);
    }
}

static void wifi_iniciar(void)
{
    eventos = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t ini = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ini));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, ao_evento, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ao_evento, NULL, NULL));

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, WIFI_SSID, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, WIFI_PASS, sizeof(cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    // o rádio dorme entre beacons por padrão; para telemetria ao vivo isso vira
    // latência visível na página
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

// ---------------------------------------------------------------- http

static esp_err_t h_pagina(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static int montar_json(char *buf, size_t tam)
{
    wifi_ap_record_t ap;
    int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;

    esp_netif_ip_info_t ip = { 0 };
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_get_ip_info(netif, &ip);
    }

    int fds[MAX_SOCKS];
    size_t n_fds = MAX_SOCKS;
    int assinantes = 0;
    if (servidor && httpd_get_client_list(servidor, &n_fds, fds) == ESP_OK) {
        for (size_t i = 0; i < n_fds; i++) {
            if (httpd_ws_get_fd_info(servidor, fds[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
                assinantes++;
            }
        }
    }

    int p = snprintf(buf, tam,
                     "{\"rgb\":[%u,%u,%u],\"max\":%d,\"brilho\":%u,\"ip\":\"" IPSTR "\",\"rssi\":%d,"
                     "\"up\":%lld,\"ws\":%d,\"wserr\":%d,\"wstx\":%lu,\"i2c\":[",
                     cor[0], cor[1], cor[2], MAXV, brilho, IP2STR(&ip.ip), rssi,
                     (esp_timer_get_time() - t0) / 1000000,
                     assinantes, ws_falhas, (unsigned long)ws_enviados);

    uint8_t n = i2c_n;
    for (uint8_t i = 0; i < n && p < (int)tam - 8; i++) {
        p += snprintf(buf + p, tam - p, i ? ",%u" : "%u", i2c_achados[i]);
    }
    p += snprintf(buf + p, tam - p,
                  "],\"bme\":%d,\"t\":%ld,\"pa\":%lu,\"rh\":%lu,\"gas\":%lu,"
                  "\"tof\":%d,\"mm\":%u,\"dst\":%u,\"sg\":%u,\"sn\":%u,\"amb\":%u,\"tn\":%u,"
                  "\"rtc\":%d,\"hr\":\"%s\",\"dt\":\"%s\",\"rtemp\":%d,\"fonte\":\"%s\","
                  "\"sd\":%d,\"sdkb\":%llu,\"umm\":%.0f,\"uok\":%d,\"ulo\":%.0f,\"uhi\":%.0f,\"uv\":%u,\"ut\":%u,\"uhz\":%.1f",
                  bme_ok ? 680 : 0,
                  (long)s_temp_c100, (unsigned long)s_press_pa, (unsigned long)s_umid_pct100,
                  (unsigned long)s_gas_ohm,
                  tof_ok ? 1 : 0, s_dist_mm, s_dist_status,
                  s_dist_sigma, s_dist_sinal, s_dist_amb, s_dist_n,
                  rtc_ok ? 1 : 0, s_hora, s_data, s_rtc_temp_c100, s_rtc_fonte,
                  sd_montado() ? 1 : 0, (unsigned long long)sd_livre_kb(),
                  s_u_mm, s_u_ok ? 1 : 0, s_u_lo, s_u_hi, s_u_val, s_u_tot, s_u_hz);

    // --- máquina. Floats saem com casa decimal já formatada: a página não deve
    //     ter que saber a escala de cada grandeza.
    static const char *NOMES[] = { "ocioso", "aquecendo", "pronta", "pré-infusão",
                                   "rampa", "platô", "declínio", "fim", "FALHA" };
    mq_estado_pub_t m;
    mq_publicar(&m);
    sim_espia_t esp;
    sim_espiar(&esp);
    mq_perfil_t pf;
    mq_perfil_get(&pf);

    p += snprintf(buf + p, tam - p,
                  ",\"m\":{\"est\":\"%s\",\"tb\":%.1f,\"pr\":%.2f,\"g\":%.1f,"
                  "\"q\":%.2f,\"niv\":%.0f,\"ap\":%.2f,\"ssr\":%.2f,\"bmb\":%.2f,"
                  "\"t\":%.1f,\"falha\":\"%s\",\"mot\":\"%s\",\"sim\":%d,"
                  "\"sp\":%.1f,\"alvo\":%.0f,\"tmax\":%.0f,"
                  "\"vaz\":%.2f,\"tagua\":%.1f,\"w\":%.0f}}",
                  NOMES[m.estado], m.t_boiler, m.pressao, m.massa, m.vazao,
                  m.nivel_pct, m.alvo_pressao, m.ssr, m.bomba, m.shot_s,
                  m.falha ? m.falha : "", m.motivo ? m.motivo : "",
                  hal_modo() == HAL_SIM ? 1 : 0,
                  pf.setpoint_c, pf.alvo_g, pf.tempo_max_s,
                  esp.vazao_g_s, esp.t_agua_grupo, esp.energia_w);
    return p;
}

// Mantido de propósito: é o teste de fumaça curl-ável. O caminho ao vivo é o
// WebSocket; este aqui é para checar a placa de um terminal sem cliente WS.
static esp_err_t h_rgb(httpd_req_t *req)
{
    char buf[800];   // folga: truncar aqui vira JSON inválido, e a página quebra calada
    int n = montar_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

// ------------------------------------------------- bancada: calibração do ToF
//
// `GET /tof`                — leitura crua atual
// `GET /tof?cal=140`        — roda a calibração de offset da ST com o alvo a 140 mm
// `GET /tof?off=-12`        — grava um offset direto, em mm
//
// A calibração de offset É a calibração normal de um ToF, ao contrário da escala
// 3,5x que o driver ULP anterior apresentava — aquilo era bug, isto é rotina.
// Procedimento da ST: alvo plano parado, refletância ~88%, distância conhecida.
//
// ⚠️ O offset fica no registrador do SENSOR, não em RAM do ESP32 — sobrevive a
// reboot e só some quando o sensor perde alimentação. Calibração ruim fica lá
// calada. Sempre conferir `sinal` depois de calibrar: offset tirado com sinal
// fraco embute viés de luz ambiente em vez do desvio real da peça.
static esp_err_t h_tof(httpd_req_t *req)
{
    char q[64], v[12];
    const char *acao = "leitura";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "cal", v, sizeof(v)) == ESP_OK) {
            int alvo = atoi(v);
            int16_t achado = 0;
            // bloqueia alguns segundos: a ST tira a média de 50 medidas
            acao = (alvo > 0 && tof_calibrar_offset((uint16_t)alvo, &achado) == ESP_OK)
                   ? "calibrado" : "falhou";
            if (achado) s_offset_mm = achado;
        } else if (httpd_query_key_value(q, "im", v, sizeof(v)) == ESP_OK) {
            // Muda só o período entre medidas, para comparar a taxa PEDIDA com a
            // taxa REAL (contador tn). Divergirem por um fator acusa o relógio.
            int ms = atoi(v);
            acao = (ms > 0 && tof_set_periodo_ms((uint16_t)ms) == ESP_OK)
                   ? "periodo" : "falhou";
        } else if (httpd_query_key_value(q, "tb", v, sizeof(v)) == ESP_OK) {
            // Orçamento de tempo. Integração mais longa = mais rejeição de luz
            // ambiente, ao custo de taxa. Valores aceitos pela ULD:
            // 15, 20, 33, 50, 100, 200, 500 ms.
            int ms = atoi(v);
            acao = (ms > 0 && tof_set_orcamento_ms((uint16_t)ms) == ESP_OK)
                   ? "orcamento" : "falhou";
        } else if (httpd_query_key_value(q, "off", v, sizeof(v)) == ESP_OK) {
            int16_t off = (int16_t)atoi(v);
            acao = (tof_set_offset_mm(off) == ESP_OK) ? "offset gravado" : "falhou";
            s_offset_mm = off;
        }
    }
    tof_get_offset_mm((int16_t *)&s_offset_mm);
    uint16_t osc = 0, modo = 0, orc = 0, per = 0;
    tof_dump(&osc, &modo, &orc, &per);
    char buf[280];
    int n = snprintf(buf, sizeof(buf),
                     "{\"acao\":\"%s\",\"off\":%d,\"mm\":%u,\"mm2\":%u,\"dst\":%u,"
                     "\"sg\":%u,\"sn\":%u,\"amb\":%u,\"tn\":%u,"
                     "\"osc\":%u,\"modo\":%u,\"orc\":%u,\"per\":%u}",
                     acao, s_offset_mm, s_dist_mm, s_dist_mm2, s_dist_status,
                     s_dist_sigma, s_dist_sinal, s_dist_amb, s_dist_n,
                     osc, modo, orc, per);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

// ------------------------------------------------- bancada: cartão e shots
//
// `GET /sd`            lista os shots gravados
// `GET /sd?f=<nome>`   devolve o CSV cru, para conferir de um terminal
static esp_err_t h_sd(httpd_req_t *req)
{
    char q[96], nome[48];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK
        && httpd_query_key_value(q, "f", nome, sizeof(nome)) == ESP_OK) {
        // Só o diretório de shots, e sem "..": endpoint de bancada não precisa
        // virar leitor de sistema de arquivos inteiro.
        if (strstr(nome, "..")) return httpd_resp_send_404(req);
        char cam[80];
        snprintf(cam, sizeof(cam), "/sd/shots/%s", nome);
        FILE *f = fopen(cam, "r");
        if (!f) return httpd_resp_send_404(req);
        httpd_resp_set_type(req, "text/csv");
        char linha[128];
        while (fgets(linha, sizeof(linha), f))
            httpd_resp_send_chunk(req, linha, HTTPD_RESP_USE_STRLEN);
        fclose(f);
        return httpd_resp_send_chunk(req, NULL, 0);
    }
    char buf[1024];
    int n = sd_listar(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}

// ---------------------------------------------------------------- websocket

// ⚠️ O handshake NÃO passa por aqui. O esp_http_server responde o 101 e retorna
// antes de chamar o handler — está escrito no httpd_uri.c: "If the request is
// websocket handshake, then do not call the uri->handler". O padrão antigo de
// registrar o fd num `if (req->method == HTTP_GET)` não dispara mais.
//
// Esta função vê os frames de dados do cliente.
//
// **Protocolo: `<comando> [valor]` em texto puro.** Continua sem JSON — analisar
// JSON no firmware para mandar um float seria trocar 3 linhas de sscanf por uma
// dependência e um buffer a mais. Um inteiro sozinho ainda vale como brilho, para
// não quebrar quem já falava com a placa.
//
//   brilho 80         shot start | shot stop      falha limpar
//   sim on | off      set <chave> <valor>         reset
//
// ⚠️ Todo valor é limitado NO SERVIDOR. O cliente não define teto — vale para o
// brilho desde o começo e agora vale para setpoint, alvo de peso e pressão, que
// são grandezas com consequência física.
//
// Drenar o frame é obrigatório mesmo que não se use: frame não lido trava o socket.
static float lim(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void ws_comando(char *s)
{
    char chave[20] = { 0 };
    float val = 0.0f;

    if (sscanf(s, "brilho %f", &val) == 1) {
        brilho = (uint8_t)lim(val, 0, MAXV);
        return;
    }
    if (strncmp(s, "shot start", 10) == 0) { mq_shot_iniciar(); return; }
    if (strncmp(s, "shot stop", 9) == 0) { mq_shot_parar(); return; }
    if (strncmp(s, "falha limpar", 12) == 0) { mq_limpar_falha(); return; }
    if (strncmp(s, "sim on", 6) == 0) { hal_set_modo(HAL_SIM); return; }
    if (strncmp(s, "sim off", 7) == 0) { hal_set_modo(HAL_REAL); return; }
    if (strncmp(s, "reset", 5) == 0) { sim_reset(); return; }

    if (sscanf(s, "set %19s %f", chave, &val) == 2) {
        mq_perfil_t p;
        mq_perfil_get(&p);
        sim_cfg_t c;
        sim_cfg_get(&c);

        if (!strcmp(chave, "sp")) p.setpoint_c = lim(val, 20, 130);
        else if (!strcmp(chave, "alvo")) p.alvo_g = lim(val, 5, 200);
        else if (!strcmp(chave, "tmax")) p.tempo_max_s = lim(val, 5, 60);
        else if (!strcmp(chave, "prebar")) p.pre_bar = lim(val, 0, 9);
        else if (!strcmp(chave, "pres")) p.pre_s = lim(val, 0, 30);
        else if (!strcmp(chave, "plato")) p.plato_bar = lim(val, 1, 12);
        else if (!strcmp(chave, "rampa")) p.rampa_s = lim(val, 0.1f, 20);
        else if (!strcmp(chave, "declinio")) p.declinio_bar = lim(val, 0, 12);
        // daqui para baixo é só simulação
        else if (!strcmp(chave, "tamb")) c.t_ambiente = lim(val, -5, 45);
        else if (!strcmp(chave, "pot")) c.pot_resistencia = lim(val, 100, 2000);
        else if (!strcmp(chave, "moagem")) c.moagem = lim(val, 0.3f, 4);
        else if (!strcmp(chave, "vel")) c.vel = lim(val, 0.1f, 20);
        else if (!strcmp(chave, "nivel")) c.nivel_inicial = lim(val, 40, 250);
        else if (!strcmp(chave, "semagua")) c.sem_agua = val > 0.5f;
        else if (!strcmp(chave, "ftemp")) c.falha_temp = val > 0.5f;
        else if (!strcmp(chave, "fpress")) c.falha_press = val > 0.5f;
        else if (!strcmp(chave, "sempuck")) c.puck_ausente = val > 0.5f;
        else return;

        mq_perfil_set(&p);
        sim_cfg_set(&c);
        return;
    }

    // compatibilidade: inteiro solto continua sendo brilho
    if (s[0] >= '0' && s[0] <= '9') brilho = (uint8_t)lim((float)atoi(s), 0, MAXV);
}

static esp_err_t h_ws(httpd_req_t *req)
{
    uint8_t buf[64] = { 0 };
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = buf };

    esp_err_t e = httpd_ws_recv_frame(req, &f, sizeof(buf) - 1);
    if (e != ESP_OK || f.len == 0) {
        return e;
    }
    buf[f.len] = '\0';
    ws_comando((char *)buf);
    return ESP_OK;
}

// Sem lista própria de clientes: o servidor já sabe quem está conectado e quem
// falou WebSocket. Perguntar a ele elimina registro, slots e limpeza de socket
// morto — e nunca fica dessincronizado.
static void tarefa_ws(void *arg)
{
    char buf[800];   // folga: truncar aqui vira JSON inválido, e a página quebra calada
    int fds[MAX_SOCKS];

    while (1) {
        size_t n_fds = MAX_SOCKS;
        if (httpd_get_client_list(servidor, &n_fds, fds) == ESP_OK) {
            int n = montar_json(buf, sizeof(buf));
            httpd_ws_frame_t f = {
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)buf,
                .len = n,
            };

            for (size_t i = 0; i < n_fds; i++) {
                if (httpd_ws_get_fd_info(servidor, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) {
                    continue;   // socket HTTP comum, não é assinante
                }
                // httpd_ws_send_frame_async faz DOIS send(): cabeçalho e depois
                // payload. Com Nagle, o segundo espera o ACK do primeiro, e o
                // delayed-ACK do outro lado segura por até ~200 ms — vira
                // travada visível. Setar a cada push é um syscall de nada e
                // evita ter de lembrar de quais fds já foram configurados.
                int um = 1;
                setsockopt(fds[i], IPPROTO_TCP, TCP_NODELAY, &um, sizeof(um));
                if (httpd_ws_send_frame_async(servidor, fds[i], &f) == ESP_OK) {
                    ws_enviados++;
                } else {
                    ws_falhas++;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000 / WS_HZ));
    }
}

static void http_iniciar(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.core_id = CORE_NET;     // servidor fora do core de tempo-real
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = MAX_SOCKS;

    ESP_ERROR_CHECK(httpd_start(&servidor, &cfg));

    httpd_uri_t u_pag = { .uri = "/", .method = HTTP_GET, .handler = h_pagina };
    httpd_uri_t u_rgb = { .uri = "/rgb", .method = HTTP_GET, .handler = h_rgb };
    httpd_uri_t u_sd = { .uri = "/sd", .method = HTTP_GET, .handler = h_sd };
    httpd_uri_t u_tof = { .uri = "/tof", .method = HTTP_GET, .handler = h_tof };
    httpd_uri_t u_ws = {
        .uri = "/ws", .method = HTTP_GET, .handler = h_ws, .is_websocket = true
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(servidor, &u_pag));
    ESP_ERROR_CHECK(httpd_register_uri_handler(servidor, &u_rgb));
    ESP_ERROR_CHECK(httpd_register_uri_handler(servidor, &u_sd));
    ESP_ERROR_CHECK(httpd_register_uri_handler(servidor, &u_tof));
    ESP_ERROR_CHECK(httpd_register_uri_handler(servidor, &u_ws));

    // push fica no core da rede: mandar frame do core 1 furaria a separação
    xTaskCreatePinnedToCore(tarefa_ws, "ws", 4096, NULL, 4, NULL, CORE_NET);
    ESP_LOGI(TAG, "http + ws no ar");
}

// ---------------------------------------------------------------- main

void app_main(void)
{
    autoteste();
    t0 = esp_timer_get_time();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    tiras[0] = criar_tira(PIN_A);
    tiras[1] = criar_tira(PIN_B);

    if (sd_iniciar() != ESP_OK) ESP_LOGW(TAG, "seguindo sem cartão");
    i2c_iniciar();
    // varredura fora do core de tempo-real: probe bloqueia por até 20 ms/endereço
    xTaskCreatePinnedToCore(tarefa_i2c, "i2c", 4096, NULL, 3, NULL, CORE_NET);
    xTaskCreatePinnedToCore(tarefa_bme, "bme", 4096, NULL, 3, NULL, CORE_NET);

    // o LED sobe antes da rede: dá feedback visual mesmo se o Wi-Fi falhar
    xTaskCreatePinnedToCore(tarefa_led, "led", 3072, NULL, 5, NULL, CORE_RT);

    // Controle no core de tempo-real e com prioridade acima do LED: se algum dia
    // faltar CPU, o que pode piscar feio é a animação, não o PID da caldeira.
    xTaskCreatePinnedToCore(tarefa_controle, "ctrl", 4096, NULL, 6, NULL, CORE_RT);

    setenv("TZ", TZ_BR, 1);
    tzset();

    wifi_iniciar();
    xEventGroupWaitBits(eventos, BIT_CONECTADO, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));

    // O servidor sobe PRIMEIRO. A task do relógio faz o NTP por conta dela — se a
    // rede estiver ruim, o painel não pode ficar esperando um horário.
    http_iniciar();
    xTaskCreatePinnedToCore(tarefa_relogio, "rtc", 4096, NULL, 2, NULL, CORE_NET);
}
