# 02 — Arquitetura elétrica e pinagem

## O MCU: ESP32-S3-WROOM-1 **N16R8**, dual USB-C, headless

**Mudança de decisão (jul/2026):** o projeto começa **sem display**, num devkit
ESP32-S3 comum que o usuário já tem. O Waveshare ESP32-S3-Touch-LCD-3.5 sai da
Fase 1 e volta na fase de interface — comprar o board não bloqueia mais nada.

Consequências de sair do board Waveshare:

| Recurso | No Waveshare | Agora |
|---|---|---|
| Display + touch | onboard | **adiado** (fase de interface) |
| RTC PCF85063 | onboard | **NTP via Wi-Fi** (não precisa de RTC) |
| Slot microSD | onboard | **módulo microSD SPI** avulso (já em mãos) |
| Expansor EXIO | onboard | não existe — sobra GPIO nativo de sobra |
| GPIOs livres | 12 | ~20 |

O UI headless da primeira fase é **Wi-Fi + web/serial**. O ESP32-S3 tem Wi-Fi, e
NTP resolve timestamp de shot sem nenhum hardware extra.

### Board confirmado

Placa em uso: **ESP32-S3-WROOM-1 N16R8**, dual USB-C
([referência](https://www.saravati.com.br/placa-esp32-s3-wroom-1-n16r8-wifi-bluetooth-dual-usb-c.html)).
Layout equivalente ao ESP32-S3-DevKitC-1.

- **N16** = 16 MB Flash · **R8** = 8 MB PSRAM **octal**
- **Dual USB-C:** uma porta é o **USB nativo** (GPIO19/20), a outra é a ponte
  **USB-UART** (console/gravação). Dá para gravar e monitorar sem gastar o USB nativo.
- **LED RGB** no GPIO48 (silk `RGB` na placa).

### ⚠️ Pinos indisponíveis nesta placa

| Faixa | Motivo |
|---|---|
| **GPIO26–32** | SPI da flash interna — nunca usar |
| **GPIO33–37** | 🔴 **PSRAM octal (R8)** — ver armadilha abaixo |
| GPIO19 / GPIO20 | USB D− / D+ (USB nativo) |
| GPIO43 / GPIO44 | UART0 — console de debug via ponte USB |
| GPIO0, 3, 45, 46 | *strapping pins* — evitar |
| GPIO48 | LED RGB onboard |

> 🔴 **Armadilha do N16R8:** os pinos **35, 36 e 37 estão serigrafados e soldáveis no
> header direito**, mas são usados internamente pela PSRAM octal. Usá-los **trava o
> boot ou corrompe a PSRAM** — e como o pino existe fisicamente, o erro parece um
> defeito de placa, não de projeto. Nenhum sinal deste projeto os utiliza.
> (Em módulos `R2`, com PSRAM quad, esses pinos seriam livres — não é o caso aqui.)

### ADC nativo: existe, mas seguimos com o ADS1115

No board Waveshare o ADS1115 era **obrigatório** (ADC1 todo ocupado, ADC2 inútil
com Wi-Fi). No devkit pelado, **ADC1 (GPIO1–10) está livre** — o argumento antigo
caiu. Mesmo assim mantemos o ADS1115, por três motivos que continuam válidos:

1. **16 bits reais** contra ~12 bits ruidosos e não-lineares do ADC do ESP32.
2. O sensor de pressão entrega **0,5–4,5 V**. O ADC do ESP32 satura em ~3,1 V —
   exigiria divisor resistivo, jogando fora resolução e adicionando deriva térmica.
   O ADS1115 alimentado em 5 V lê o sinal **direto**.
3. Já está comprado.

## Alimentação

```
AC ──► HLK-5M05 (isolada, 5 V / 1 A)
         │
         ├─► 5 V ──► ADS1115 (VDD)  ──► sensor de pressão (VCC)
         │            └─► [ level shifter ] ──► I²C 3,3 V do ESP32
         │
         └─► 5 V ──► ESP32-S3 (pino 5V/VIN, regulador onboard faz 3,3 V)
                      └─► 3,3 V ──► MAX31865, módulo microSD
```

- **Capacitor de 470–1000 µF no trilho de 5 V, junto ao ESP32.** O HLK-5M05 entrega
  1 A contínuo, mas os bursts de TX do Wi-Fi passam disso por alguns
  milissegundos. Sem o bulk, o sintoma é brownout aleatório no boot ou ao conectar.
- **MAX31865 e módulo microSD em 3,3 V** — são periféricos SPI de 3,3 V, ligam
  direto no ESP32 sem shifter.
- **ADS1115 em 5 V** — precisa do level shifter no I²C (ver abaixo).

### O level shifter no I²C

O ADS1115 roda em 5 V para ter headroom nos 4,5 V do sensor de pressão. Isso
coloca o barramento I²C dele em nível 5 V, que **não** é seguro nos pinos do
ESP32-S3. Solução: módulo level shifter bidirecional (MOSFET) entre os dois lados.

- Lado LV → 3,3 V do ESP32 (SDA/SCL).
- Lado HV → 5 V do ADS1115.
- Os pull-ups de 10 k que já vêm no módulo ADS1115 ficam no lado HV. **É a
  configuração normal desse tipo de shifter — não remova.** O lado LV precisa dos
  pull-ups próprios do módulo shifter (a maioria já traz).

## Mapa de pinos do projeto

> ⚠️ Confira contra o silk do **seu** devkit — a numeração varia entre clones.

| Sinal | GPIO | Tipo | Observação |
|---|---|---|---|
| I²C SDA | **GPIO8** | I²C (3,3 V) | → level shifter → ADS1115 |
| I²C SCL | **GPIO9** | I²C (3,3 V) | idem |
| SPI SCLK | **GPIO12** | SPI (bus compartilhado) | MAX31865 + microSD |
| SPI MOSI | **GPIO11** | SPI | |
| SPI MISO | **GPIO13** | SPI | |
| CS MAX31865 | **GPIO10** | out | sensor do boiler |
| CS microSD | **GPIO14** | out | módulo microSD |
| UART TX → DimmerLink | **GPIO4** | out | comando da bomba, 115200 8N1 |
| UART RX ← DimmerLink | **GPIO5** | in | resposta/ACK |
| SSR (resistência) | **GPIO6** | out | PID de temperatura |
| Flow meter (futuro) | **GPIO7** | in / interrupção | sensor Hall |
| Livres | GPIO1, 2, 15, 16, 17, 18, 21, 38–42, 47 | — | folga grande |

Todos os pinos usados estão no **header esquerdo**, em sequência (4–14) — conveniente
para o chicote e para a peça 3D.

### SPI compartilhado entre MAX31865 e microSD

Funciona, e é o caminho mais curto: mesmo SCLK/MOSI/MISO, CS separados.

O detalhe que importa: o **MAX31865 é SPI modo 1** (CPOL=0, CPHA=1) e o **cartão SD
é modo 0**. Tanto o driver SPI do ESP-IDF quanto `SPI.beginTransaction()` do
Arduino trocam o modo por transação, então a convivência é transparente — desde
que **todo acesso** seja feito dentro de uma transação com o `SPISettings` correto.
Acesso "solto" fora de transação é o bug clássico aqui.

### Canais do ADS1115

| Canal | Sinal | Situação |
|---|---|---|
| A0 | Sensor de pressão (0,5–4,5 V) | agora |
| A1 | ACS712 corrente da resistência | futuro (diagnóstico) |
| A2 | ACS712 corrente da bomba | futuro (diagnóstico) |
| A3 | livre | — |

- **Endereço:** 0x48 (ADDR→GND).
- **PGA: ±6,144 V.** Obrigatório — em qualquer ganho maior o sinal de 4,5 V satura.
  Resolução resultante: 187,5 µV/LSB → ~0,00065 bar por contagem. O ruído do sensor
  limita muito antes do ADC.
- Rode um **I²C scan** no primeiro boot para confirmar o endereço.

## Controle da bomba: dimmer RobotDyn com **DimmerLink** integrado

O módulo comprado (variante "8A UART-I²C V2") traz o **DimmerLink** integrado —
um controlador dedicado (Cortex-M+) que faz **toda** a parte crítica de tempo:
detecção de zero-cross, cálculo do ângulo de fase e disparo do TRIAC.

Normalmente o DimmerLink é uma placa **separada** entre o MCU e o módulo TRIAC:

```
MCU ──UART/I²C──► DimmerLink ──(Z-C + Dim)──► módulo TRIAC ──► carga
```

Na variante integrada os dois últimos estágios vêm na mesma placa, então o ESP32 só
conversa por **UART ou I²C**. Isso muda três coisas no projeto:

1. **Não existe mais pino de zero-cross nem de gate no ESP32.** Os dois GPIOs que
   estavam reservados para isso foram realocados para a UART do DimmerLink.
2. **Nenhuma ISR de timing crítico no firmware.** Era o maior risco de jitter do
   laço de controle — some inteiro.
3. **Aceita 1,8 / 3,3 / 5 V direto**, sem level converter.

### UART ou I²C?

**Escolhido: UART** (GPIO4/GPIO5, 115200 8N1).

O I²C funcionaria — o DimmerLink responde em **0x50**, sem conflito com o ADS1115
(0x48). Mas o barramento I²C aqui já atravessa um level shifter e carrega o ADC de
pressão, que é lido em alta taxa durante o shot. Manter o atuador da bomba num canal
próprio deixa os dois subsistemas independentes para depurar, e sobra GPIO de sobra.
Comandos de 3–4 bytes a 115200 levam ~300 µs — irrelevante.

> Se optar por I²C depois, note que o DimmerLink pede **100 kHz**; o barramento
> inteiro teria que rodar nessa velocidade.

### Curva de dimerização

O DimmerLink oferece curvas **Linear**, **RMS** e **Logarítmica**, com auto-detecção
de 50/60 Hz (Brasil = 60 Hz, resolvido sozinho).

➡️ **Começar pela curva RMS.** "Linear" é linear no *ângulo de fase*, que não tem
relação útil com o que a bomba faz. A curva RMS mapeia o comando para **tensão RMS**,
que é muito mais próxima da grandeza que move um solenoide — ou seja, lineariza o
processo antes do PID e facilita a sintonia. Confirmar empiricamente na
caracterização ([07](07-seguranca-e-calibracao.md)).

### A confirmar no módulo em mãos

- [ ] Quais pads estão expostos: UART, I²C, ou ambos.
- [ ] Endereço I²C = 0x50 (se for usar I²C).
- [ ] Conjunto de comandos exato — a documentação vive em
      [github.com/robotdyn-dimmer/DimmerLink](https://github.com/robotdyn-dimmer/DimmerLink)
      (guias 01–08 + exemplos em Arduino/Python/MicroPython).
- [ ] ⚠️ **Não** usar a biblioteca `RBDDimmer` — ela é do dimmer clássico, com pinos
      ZC/PWM. Aqui não há biblioteca: são bytes crus na serial.

## Sensor de temperatura: PT100 3 fios + MAX31865

**Mudança de decisão (jul/2026): PT100, não PT1000.** Justificativa completa em
[01](01-projeto-visao-geral.md#decisões-de-arquitetura-e-o-porquê).

- **Rref do módulo = 430 Ω.** Confirme pela marcação do resistor SMD:
  `4300` ou `431` → 430 Ω → **PT100** ✅ · `4301` ou `432` → 4300 Ω → PT1000 ❌
- **Configurar o módulo para 3 fios** — nas placas estilo Adafruit são pads de
  solda que precisam ser reconfigurados. A placa vem de fábrica em 2/4 fios.
- Um sensor só (boiler). O segundo sensor foi **removido do projeto** — ver
  [01](01-projeto-visao-geral.md) e o procedimento de offset em [07](07-seguranca-e-calibracao.md).

## Lado AC

```
                    ┌──────────────────────────────────────────┐
   AC 127/220V ─────┤ FUSÍVEL TÉRMICO ~165 °C (MANTER INTACTO) │
                    └───┬──────────┬───────────┬───────────────┘
                        │          │           │
                  ┌─────▼────┐ ┌───▼────┐ ┌────▼──────────┐  ┌──────────┐
                  │HLK-5M05  │ │SSR-40DA│ │ Dimmer 8A     │  │ (chassi) │
                  │  AC→5V   │ │+dissip.│ │ +DimmerLink   │  │  ATERRAR │
                  └─────┬────┘ └───┬────┘ └────┬──────────┘  └──────────┘
                        │5V        │AC         │AC
                        │    ┌─────▼──────┐    │
                        │    │ TERMOSTATO │    │  ← backstop de hardware
                        │    │ VAPOR ~140°│    │    (ver docs/07)
                        │    └─────┬──────┘    │
                        │      ┌───▼────┐ ┌────▼─────┐
                        │      │Resist. │ │  Bomba   │
                        │      └────────┘ └──────────┘
       ┌────────────────▼──────────────────────────────────┐
       │        ESP32-S3-WROOM-1 N16R8 (headless)           │
       │  I²C(8/9)──[shifter]──ADS1115 0x48──[Pressão]      │
       │  SPI(11/12/13)──MAX31865──[PT100 boiler 3 fios]    │
       │              └──microSD (logging)                  │
       │  GPIO6──►SSR      UART(4/5)──►DimmerLink──►bomba   │
       │  Wi-Fi (NTP + web UI + telemetria)                 │
       └────────────────────────────────────────────────────┘
```

**Aterramento:** GND comum entre ESP32, ADS1115, MAX31865, módulo SD e as entradas
de controle do SSR e do dimmer. O lado AC fica isolado pelos optoacopladores do SSR
e do dimmer e pela fonte isolada Hi-Link. Chassi metálico **aterrado**.

## Pontos a verificar no protótipo

1. **I²C scan** através do level shifter — se o ADS1115 não aparecer em 0x48, o
   suspeito nº1 é pull-up/alimentação no lado HV do shifter.
2. **Rref do MAX31865 = 430 Ω** e **jumper de 3 fios** configurado.
3. **PGA do ADS1115 em ±6,144 V** antes de ligar o sensor de pressão.
4. **DimmerLink respondendo na UART** antes de ligar qualquer carga AC — testar com
   lâmpada e varrer 0→100%.
5. **Nenhum sinal em GPIO33–37.** Se o boot travar ou a PSRAM falhar, é o primeiro
   lugar a olhar.
6. **Brownout no boot** → falta o bulk de 470–1000 µF no 5 V.
7. Acesso SPI sempre dentro de `beginTransaction` com o modo correto por dispositivo.

---

## Apêndice — board com display (fase 3)

Quando a interface local entrar, o alvo continua sendo o **Waveshare
ESP32-S3-Touch-LCD-3.5, variante SPI/ST7796** (não a QSPI/AXS15231B: o ST7796 tem
driver LVGL muito mais maduro, e os pinos livres no header são os mesmos).

Referências offline já levantadas, guardadas em `docs/assets/`:
[tabela de GPIO](assets/waveshare-gpio-table.png) ·
[esquemático completo](assets/waveshare-esp32s3-lcd35-schematic.pdf).

⚠️ **A pinagem deste documento não se transfere direto para aquele board.** Lá,
ADC1 e ADC2 são inutilizáveis (LCD/I²C/SD ocupam ADC1; ADC2 conflita com Wi-Fi), o
I²C é compartilhado em IO8/IO7 com touch/RTC/IMU/PMIC, e sobram só 12 GPIOs nativos
(IO17, 18, 21, 38–42, 45–48). Na migração, o ADS1115 volta a ser **obrigatório** —
e é justamente por isso que ele já está no projeto agora.
