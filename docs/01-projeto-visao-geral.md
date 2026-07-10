# 01 — Visão geral do projeto

## Objetivo

Digitalizar e automatizar por completo uma cafeteira espresso **Philco 15 Bar**,
substituindo o controle mecânico original por um sistema baseado em **ESP32-S3**
com:

- Medição **digital** de temperatura e pressão
- Controle **digital** de temperatura (PID) e de pressão (malha fechada)
- **Display touch** local (controle sem depender de celular/PC)
- **Wi-Fi / web app** opcional na rede local (e possivelmente BLE)
- **Logging** de cada extração (shot) e visualização em gráficos
- Recursos avançados: perfis de pré-infusão e *pressure profiling*, flush, backflush, steam, OTA

A meta não é só "modificar", é produzir um **manual de engenharia reproduzível**
(esquemas, BOM, firmware, documentação) — algo acima dos mods existentes da Philco,
que hoje usam PID comerciais (Inkbird, REX-C100, XMT7100) e nenhum microcontrolador moderno.

## A máquina

- **Modelo:** Philco 15 Bar (mesma plataforma da Philco 20 Bar — mesma carcaça, boiler, hidráulica e grupo; mudam bomba/termostato de vapor).
- **Tipo:** single boiler, bomba vibratória (ULKA ou clone), boiler de alumínio com resistência embutida, termostatos bimetálicos (café ~95°C / vapor ~140°C) e **fusível térmico** de segurança.
- **Alimentação:** 127/220V.

### Como ela funciona (original)

```
Reservatório → Bomba vibratória → (OPV?) → Boiler → Grupo → Porta-filtro
```

- Temperatura: controlada por **termostato mecânico** (liga/desliga por histerese). **Não há PID.**
- Pressão: **não é controlada** — a bomba gera pressão bruta; quem limita é a resistência do café e a OPV (se existir).

### Modificações atuais (a serem removidas)

- Manômetro analógico de 12 bar, ligado via TEE na linha de pressão (**rosca 1/8" NPT**, mangueira **4mm**).
- Dimmer controlando a **bomba** (não a resistência). Problema relatado: não faz ajuste fino; raramente chega a 9 bar (trava em ~11–12 bar). Causas prováveis: resposta **não-linear** da bomba vibratória, OPV abrindo, e perda de torque do dimmer por ângulo de fase.

> Ambas as modificações serão **substituídas** pela nova eletrônica.

## Decisões de arquitetura (e o porquê)

| Decisão | Escolha | Motivo |
|---|---|---|
| MCU + display | Waveshare ESP32-S3-Touch-LCD-3.5 (SPI/ST7796) | Display **SPI** (não RGB paralelo) → sobra GPIO; traz RTC + microSD + 8MB PSRAM; driver LVGL maduro. Ver [02](02-arquitetura-eletrica-pinout.md). |
| Sensor de temperatura | PT1000 classe A + MAX31865 (×2) | Maior resolução e menor sensibilidade ao cabo que PT100; padrão em espresso. Dois pontos: **boiler** (controle) e **saída do grupo** (só monitorar temperatura real na água). |
| Leitura analógica | **ADS1115 (ADC por I²C)** | O ESP32-S3 **não tem ADC utilizável livre** neste board com Wi-Fi ligado (ver [02](02-arquitetura-eletrica-pinout.md)). Obrigatório para ler o sensor de pressão. |
| Sensor de pressão | 0–16 bar, saída 0,5–4,5V, 1/8" NPT | Mais simples que 4–20mA; faixa cobre os ~15 bar da máquina. |
| Controle da resistência | SSR-40DA | Chaveamento silencioso, sem desgaste; PID por software liga/desliga dezenas de vezes/min. |
| Controle da bomba | Dimmer AC com zero-cross + TRIAC (malha fechada) | Permite *pressure profiling*; malha fechada corrige a não-linearidade da bomba. Ver [07](07-seguranca-e-calibracao.md). |
| Hidráulica | Padronizada em **1/8" NPT**, mangueira 4mm | Casa com o manômetro/TEE que o usuário já possui. |

## Fases do projeto

1. **Documentação + hardware (Fase 1) ✅** — esta documentação.
2. **Engenharia reversa física** — abrir a máquina, confirmar bomba/OPV/rosca ([06](06-engenharia-reversa-checklist.md)).
3. **Instrumentação** — instalar sensores.
4. **Controle** — PID de temperatura + malha de pressão.
5. **Firmware** — arquitetura modular, UI, API ([05](05-firmware-arquitetura.md)).
6. **Interfaces** — touchscreen + web UI.
7. **Avançado** — perfis, gráficos em tempo real, histórico, OTA, (opcional) Home Assistant.

## Estratégia de montagem

- Protótipo com **módulos "voando"** conectados, presos por uma **peça impressa 3D** que organiza tudo.
- Firmware desenhado **como se já fosse uma PCB** desde o início (separação lógica entre "Control Board" — sensores/atuadores/segurança/tempo-real — e "UI Board" — touchscreen/Wi-Fi/API), mesmo rodando num único ESP32.
- Quando o protótipo estiver validado, migrar para **PCB** (KiCad) reaproveitando o mapa de pinos deste documento.
