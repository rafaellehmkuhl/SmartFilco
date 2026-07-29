// SmartFilco — bring-up em ESP-IDF
// RGB WS2812 em passeio semi-aleatório + página web mostrando a cor ao vivo.
//
// Divisão de cores segue a docs/05: core 1 = tempo-real (aqui, a animação do
// LED, que depois vira o laço de controle), core 0 = Wi-Fi/HTTP.

#include <assert.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
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

#define MAXV 80          // brilho (V). WS2812 no máximo ofusca e puxa ~60 mA
#define SAT 255          // saturação cheia — é o que impede o cinza
#define FRAME_MS 20      // 50 fps
#define TRANSICAO_MS 3000
#define CORE_RT 1        // core do tempo-real
#define CORE_NET 0       // core da rede
// httpd_get_client_list() devolve ESP_ERR_INVALID_ARG se houver mais sockets
// abertos que o array comporta — por isso o mesmo valor governa os dois.
#define MAX_SOCKS 7      // = httpd_config_t.max_open_sockets
#define WS_HZ 30         // taxa de push do estado (33 ms)

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static led_strip_handle_t tiras[2];
static int64_t t0;
static httpd_handle_t servidor;
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
        hsv_para_rgb(matiz, SAT, MAXV, cor);

        for (int t = 0; t < 2; t++) {
            ESP_ERROR_CHECK(led_strip_set_pixel(tiras[t], 0, cor[0], cor[1], cor[2]));
            ESP_ERROR_CHECK(led_strip_refresh(tiras[t]));
        }

        // Heartbeat. Com USB-Serial-JTAG não dá para capturar o boot de forma
        // confiável (o reset derruba a própria USB), então o firmware precisa
        // falar sozinho — senão a placa fica indepurável sem monitor interativo.
        if (++quadros % (2000 / FRAME_MS) == 0) {
            ESP_LOGI(TAG, "matiz=%u  rgb=%u,%u,%u  heap=%lu  core=%d",
                     matiz, cor[0], cor[1], cor[2],
                     (unsigned long)esp_get_free_heap_size(), xPortGetCoreID());
        }

        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
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

    return snprintf(buf, tam,
                    "{\"rgb\":[%u,%u,%u],\"max\":%d,\"ip\":\"" IPSTR "\",\"rssi\":%d,"
                    "\"up\":%lld,\"ws\":%d,\"wserr\":%d,\"wstx\":%lu}",
                    cor[0], cor[1], cor[2], MAXV, IP2STR(&ip.ip), rssi,
                    (esp_timer_get_time() - t0) / 1000000,
                    assinantes, ws_falhas, (unsigned long)ws_enviados);
}

// Mantido de propósito: é o teste de fumaça curl-ável. O caminho ao vivo é o
// WebSocket; este aqui é para checar a placa de um terminal sem cliente WS.
static esp_err_t h_rgb(httpd_req_t *req)
{
    char buf[192];
    int n = montar_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, n);
}

// ---------------------------------------------------------------- websocket

// ⚠️ O handshake NÃO passa por aqui. O esp_http_server responde o 101 e retorna
// antes de chamar o handler — está escrito no httpd_uri.c: "If the request is
// websocket handshake, then do not call the uri->handler". O padrão antigo de
// registrar o fd num `if (req->method == HTTP_GET)` não dispara mais.
//
// Esta função só vê frames de dados. A página não manda nenhum, mas um frame
// que chegue e não seja drenado trava o socket.
static esp_err_t h_ws(httpd_req_t *req)
{
    uint8_t lixo[64];
    httpd_ws_frame_t f = { .type = HTTPD_WS_TYPE_TEXT, .payload = lixo };
    return httpd_ws_recv_frame(req, &f, sizeof(lixo));
}

// Sem lista própria de clientes: o servidor já sabe quem está conectado e quem
// falou WebSocket. Perguntar a ele elimina registro, slots e limpeza de socket
// morto — e nunca fica dessincronizado.
static void tarefa_ws(void *arg)
{
    char buf[192];
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
    httpd_uri_t u_ws = {
        .uri = "/ws", .method = HTTP_GET, .handler = h_ws, .is_websocket = true
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(servidor, &u_pag));
    ESP_ERROR_CHECK(httpd_register_uri_handler(servidor, &u_rgb));
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

    // o LED sobe antes da rede: dá feedback visual mesmo se o Wi-Fi falhar
    xTaskCreatePinnedToCore(tarefa_led, "led", 3072, NULL, 5, NULL, CORE_RT);

    wifi_iniciar();
    xEventGroupWaitBits(eventos, BIT_CONECTADO, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    http_iniciar();
}
