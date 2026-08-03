// microSD por SPI + gravação de shot em CSV.
//
// ⚠️ **A pergunta do MISO continua aberta** ([docs/02]). Hoje o cartão é o único
// no barramento e funciona sozinho de qualquer jeito; o risco aparece quando o
// MAX31865 entrar (05/08). Muito módulo barato não solta o MISO com o CS inativo,
// e aí o MAX31865 não consegue responder — falha traiçoeira porque cada um
// funciona isolado. **Medir antes de montar o chicote.**

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t sd_iniciar(void);
bool sd_montado(void);
uint64_t sd_livre_kb(void);

// Um arquivo por shot, nomeado pelo relógio. Se o RTC não tiver hora com
// procedência, cai para o uptime — nome feio é melhor que carimbo mentiroso.
esp_err_t sd_shot_abrir(void);
void sd_shot_linha(float t_s, float t_boiler, float pressao, float alvo_p,
                   float massa, float vazao, float ssr, float bomba);
void sd_shot_fechar(const char *motivo);
const char *sd_shot_arquivo(void);

// Lista os shots gravados como JSON. Sem isto o cartão é uma caixa preta: dá
// para saber que montou, não que gravou.
int sd_listar(char *buf, int tam);
