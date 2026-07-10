# 02 — Arquitetura elétrica e pinagem

## O board: Waveshare ESP32-S3-Touch-LCD-3.5 (variante SPI/ST7796)

Especificações relevantes:

- **ESP32-S3R8** dual-core LX7 @ 240 MHz, Wi-Fi 2,4 GHz + BLE 5
- **8 MB PSRAM** + **16 MB Flash**
- Display **IPS 3,5" 320×480**, driver **ST7796 via SPI**, touch capacitivo **FT6336 via I²C**
- Onboard: **RTC PCF85063**, **IMU QMI8658**, **PMIC AXP2101**, **codec de áudio**, **slot microSD**, **interface de câmera**, **expansor de I/O (EXIO)**
- USB-C, botões PWR e BOOT programáveis

> **Por que SPI e não QSPI:** a versão QSPI (AXS15231B) tem mais FPS, útil só para
> vídeo/animação em tela cheia — irrelevante para esta UI. O ST7796/SPI tem driver
> LVGL muito mais maduro. Os pinos livres no header são os mesmos nas duas versões.

## Mapa de GPIO (extraído do esquemático oficial Waveshare)

> Referências offline salvas em `docs/assets/`:
> [tabela de GPIO](assets/waveshare-gpio-table.png) e
> [esquemático completo](assets/waveshare-esp32s3-lcd35-schematic.pdf).

### Pinos já ocupados por periféricos onboard

| Função | GPIO |
|---|---|
| LCD SPI: MOSI / MISO / DC / SCLK / BL | IO1 / IO2 / IO3 / IO5 / IO6 |
| **I²C compartilhado** (SDA / SCL) | **IO8 / IO7** — touch, RTC, IMU, AXP2101, expansor EXIO |
| SD card SPI: MISO / MOSI / SCLK | IO9 / IO10 / IO11 |
| Codec I²S | IO12–IO16 |
| Câmera (não usada) | IO17,18,21,38–42,45–48 (compartilháveis, ver abaixo) |
| USB D-/D+ | IO19 / IO20 |
| UART debug | IO43 (TX) / IO44 (RX) |
| LCD_RST, TP_INT, SD_CS, RTC_INT, etc. | no **expansor EXIO** (não gastam GPIO nativo) |

### ⚠️ Não há ADC nativo utilizável

- ADC1 (GPIO1–10): **todos ocupados** (LCD/I²C/SD).
- ADC2 (GPIO11–20): **inutilizável com Wi-Fi ligado** (limitação do ESP32-S3).

➡️ **Toda leitura analógica passa pelo ADS1115 (ADC 16-bit por I²C).** Isso não é opcional.

### 12 GPIOs nativos livres no header

Como **não usaremos a câmera**, ficam livres:

> **IO17, IO18, IO21, IO38, IO39, IO40, IO41, IO42, IO45, IO46, IO47, IO48**

## Atribuição de pinos do projeto

| Sinal | Pino | Tipo | Observação |
|---|---|---|---|
| I²C SDA (ADS1115) | **GPIO8** | I²C | barramento existente (pull-ups no board) |
| I²C SCL (ADS1115) | **GPIO7** | I²C | idem |
| SPI sensores — SCLK | **GPIO38** | SPI (novo bus dedicado) | para os 2× MAX31865 |
| SPI sensores — MOSI (SDI) | **GPIO39** | SPI | |
| SPI sensores — MISO (SDO) | **GPIO40** | SPI | |
| CS MAX31865 boiler | **GPIO41** | out | PT1000 #1 |
| CS MAX31865 grupo | **GPIO42** | out | PT1000 #2 |
| Zero-cross (bomba) | **GPIO17** | in / interrupção | do módulo dimmer |
| TRIAC gate (bomba) | **GPIO18** | out | ao módulo dimmer |
| SSR (resistência) | **GPIO21** | out | PID de temperatura |
| Flow meter (futuro) | **GPIO45** | in / interrupção | sensor Hall |
| Reserva | **GPIO46, 47, 48** | — | 3 pinos sobrando |

> GPIO17/18 são tecnicamente ADC2, mas os usamos como **digital** (zero-cross/TRIAC),
> então o conflito ADC2 × Wi-Fi não afeta.

### Canais do ADS1115

| Canal | Sinal | Situação |
|---|---|---|
| A0 | Sensor de pressão (0,5–4,5V) | agora |
| A1 | ACS712 corrente da resistência | futuro (diagnóstico) |
| A2 | ACS712 corrente da bomba | futuro (diagnóstico) |
| A3 | livre | — |

- **Endereço I²C do ADS1115:** 0x48 (ADDR→GND). Sem conflito com os chips onboard (AXP2101 0x34, FT6336 ~0x38/0x5D, QMI8658 0x6A/0x6B, PCF85063 0x51, expansor ~0x20). Rode um **I²C scan** no primeiro boot para confirmar.
- Alimente o **ADS1115 com 5V** para ter headroom nos 4,5V do sensor de pressão. O I²C é open-drain, então conviver com os pull-ups de 3,3V do barramento é ok (linhas SDA/SCL do ADS1115 são tolerantes).

## Diagrama elétrico (visão geral)

```
                          ┌───────────────────────────────┐
        AC 127/220V ──────┤ FUSÍVEL TÉRMICO (manter!)      │
                          └──────┬─────────────┬──────────┘
                                 │             │
                    ┌────────────┤             ├───────────────┐
                    │            │             │               │
              ┌─────▼─────┐  ┌───▼────┐   ┌────▼─────┐   ┌──────▼──────┐
              │ HLK-5M05  │  │ SSR-40 │   │  Dimmer  │   │  (chassi)   │
              │  AC→5V    │  │  DA    │   │ AC c/ ZC │   │  ATERRAR    │
              └─────┬─────┘  └───┬────┘   └────┬─────┘   └─────────────┘
                    │ 5V         │AC           │AC
                    │        ┌───▼────┐   ┌────▼─────┐
                    │        │Resist. │   │  Bomba   │
                    │        └────────┘   └──────────┘
       ┌────────────▼───────────────────────────────────────┐
       │                 ESP32-S3 (Waveshare 3.5")           │
       │  I²C(7/8)──ADS1115──[Pressão, correntes]            │
       │  SPI(38/39/40)──MAX31865×2──[PT1000 boiler/grupo]   │
       │  GPIO21──►SSR   GPIO18──►TRIAC   GPIO17◄──ZeroCross │
       │  microSD(logging)   RTC   Wi-Fi/BLE   Touch LVGL    │
       └─────────────────────────────────────────────────────┘
```

**Regra de ouro do aterramento:** GND comum entre ESP32, ADS1115, MAX31865 e as
entradas de controle do SSR e do dimmer. O lado AC fica **isolado** pelos
optoacopladores do SSR e do dimmer + a fonte isolada Hi-Link. Chassi metálico **aterrado**.

## Pontos a verificar no protótipo

1. **I²C scan** confirmando ADS1115 em 0x48 e nenhum conflito.
2. **Dimmer RobotDyn a 3,3V:** o gate (LED do MOC3021) puxa ~10 mA; a 3,3V com ~220Ω dá ~9,5 mA — normalmente funciona, mas confirme o disparo. Se falhar, usar transistor/level-shifter para 5V.
3. **MAX31865 configurado para PT1000** (Rref = 4300Ω nas placas de PT1000; conferir jumper 2/3/4 fios da sonda).
4. **Compartilhamento do SPI:** o bus dos MAX31865 é **dedicado** (GPIO38/39/40), separado do SPI do display e do SD — sem conflito de modo SPI.
