// HC-SR04 no reservatório. Ver ultra.c para as armadilhas (ECHO de 5 V, zona
// cega, por que não é o AJ-SR04T, e por que mediana e não média).

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    float mm;          // MEDIANA das amostras válidas da janela
    float min_mm;      // espalhamento: mediana sozinha esconde o que se investiga
    float max_mm;
    uint8_t validas;   // quantas da janela passaram
    uint8_t total;
    bool valida;       // houve amostra suficiente para a mediana significar algo
    uint32_t ecos;     // acumulado desde o boot; parado = sensor mudo
    float hz;          // taxa medida, não a pedida
} ultra_med_t;

esp_err_t ultra_iniciar(void);
void ultra_ler(ultra_med_t *o);

// Silencia a amostragem durante o OTA — ver ads.c.
void ultra_pausar(bool p);

// Temperatura ambiente para corrigir a velocidade do som (~0,6 m/s por °C).
void ultra_set_temp(float c);
