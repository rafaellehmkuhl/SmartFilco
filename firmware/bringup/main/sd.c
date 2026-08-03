#include "sd.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";

// Mesmos pinos que a docs/02 reserva ao projeto — prototipar no pino definitivo
// evita refazer chicote depois.
#define PIN_MOSI 11
#define PIN_SCK 12
#define PIN_MISO 13
#define PIN_CS 14
#define MONTAGEM "/sd"

// 20 MHz é conservador de propósito. Módulo avulso com fio de protoboard não
// sustenta os 40 MHz do padrão, e o sintoma de SPI rápido demais é arquivo
// corrompido — que só aparece na hora de ler o log, quando o shot já passou.
#define SPI_HZ (10 * 1000)   // 10 MHz: 20 é otimista em fio de protoboard

static sdmmc_card_t *cartao;
static bool montado;
static FILE *f_shot;
static char nome_shot[48];
static uint32_t linhas;

esp_err_t sd_iniciar(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t e = spi_bus_initialize(SPI2_HOST, &bus, SDSPI_DEFAULT_DMA);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(e));
        return e;
    }

    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = PIN_CS;
    dev.host_id = SPI2_HOST;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = SPI_HZ;

    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        // NÃO formatar em caso de falha. Cartão que não monta pode ser fiação, e
        // formatar por engano apaga log de shot que não volta.
        .format_if_mount_failed = false,
        .max_files = 3,
        .allocation_unit_size = 16 * 1024,
    };

    e = esp_vfs_fat_sdspi_mount(MONTAGEM, &host, &dev, &cfg, &cartao);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "não montou (%s) — confira fiação, formatação FAT e CS",
                 esp_err_to_name(e));
        return e;
    }
    montado = true;
    ESP_LOGI(TAG, "montado: %s %lluMB", cartao->cid.name,
             ((uint64_t)cartao->csd.capacity) * cartao->csd.sector_size / (1024 * 1024));
    mkdir(MONTAGEM "/shots", 0777);
    return ESP_OK;
}

bool sd_montado(void) { return montado; }

uint64_t sd_livre_kb(void)
{
    if (!montado) return 0;
    FATFS *fs;
    DWORD livres;
    if (f_getfree("0:", &livres, &fs) != FR_OK) return 0;
    return (uint64_t)livres * fs->csize * 512 / 1024;
}

const char *sd_shot_arquivo(void) { return f_shot ? nome_shot : ""; }

esp_err_t sd_shot_abrir(void)
{
    if (!montado || f_shot) return ESP_ERR_INVALID_STATE;

    time_t agora = time(NULL);
    struct tm t;
    localtime_r(&agora, &t);
    if (t.tm_year > 100) {
        strftime(nome_shot, sizeof(nome_shot), MONTAGEM "/shots/%Y%m%d-%H%M%S.csv", &t);
    } else {
        // Sem hora com procedência, carimbar data seria inventar. Uptime é feio e
        // honesto — e ordena certo dentro do mesmo boot, que é o que importa.
        snprintf(nome_shot, sizeof(nome_shot), MONTAGEM "/shots/up%llu.csv",
                 (unsigned long long)(esp_timer_get_time() / 1000000));
    }

    f_shot = fopen(nome_shot, "w");
    if (!f_shot) {
        ESP_LOGE(TAG, "fopen %s falhou", nome_shot);
        return ESP_FAIL;
    }
    linhas = 0;
    // Colunas conforme a docs/05, mais o alvo de pressão: sem ele o gráfico
    // mostra o que aconteceu mas não o que era para acontecer.
    fprintf(f_shot, "t_s,t_boiler_c,pressao_bar,alvo_bar,massa_g,vazao_g_s,ssr,bomba\n");
    ESP_LOGI(TAG, "gravando %s", nome_shot);
    return ESP_OK;
}

void sd_shot_linha(float t_s, float t_boiler, float pressao, float alvo_p,
                   float massa, float vazao, float ssr, float bomba)
{
    if (!f_shot) return;
    fprintf(f_shot, "%.2f,%.2f,%.3f,%.2f,%.2f,%.3f,%.3f,%.3f\n",
            t_s, t_boiler, pressao, alvo_p, massa, vazao, ssr, bomba);
    // fflush a cada ~1 s (20 linhas a 20 Hz). Sem isso, queda de energia no meio
    // do shot leva o buffer junto — e queda no meio do shot é justamente o caso
    // que a gente mais vai querer ler depois.
    if (++linhas % 20 == 0) fflush(f_shot);
}

void sd_shot_fechar(const char *motivo)
{
    if (!f_shot) return;
    fprintf(f_shot, "# fim: %s\n", motivo ? motivo : "");
    fclose(f_shot);
    f_shot = NULL;
    ESP_LOGI(TAG, "%s fechado, %lu linhas (%s)", nome_shot,
             (unsigned long)linhas, motivo ? motivo : "");
}

int sd_listar(char *buf, int tam)
{
    int p = snprintf(buf, tam, "{\"montado\":%d,\"livre_kb\":%llu,\"shots\":[",
                     montado ? 1 : 0, (unsigned long long)sd_livre_kb());
    if (!montado) return p + snprintf(buf + p, tam - p, "]}");

    DIR *d = opendir(MONTAGEM "/shots");
    if (!d) return p + snprintf(buf + p, tam - p, "]}");

    struct dirent *ent;
    int n = 0;
    while ((ent = readdir(d)) && p < tam - 90) {
        if (ent->d_name[0] == '.') continue;
        // 300: d_name é de 256 no FATFS, e o compilador reclama com razão se o
        // destino não couber o pior caso.
        char cam[300];
        snprintf(cam, sizeof(cam), MONTAGEM "/shots/%s", ent->d_name);
        struct stat st;
        long bytes = (stat(cam, &st) == 0) ? (long)st.st_size : -1;
        p += snprintf(buf + p, tam - p, "%s{\"n\":\"%s\",\"b\":%ld}",
                      n++ ? "," : "", ent->d_name, bytes);
    }
    closedir(d);
    return p + snprintf(buf + p, tam - p, "]}");
}
