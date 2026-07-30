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
| **Z-C** (zero-cross) | **GPIO4** | in / interrupção | do dimmer. ISR em **IRAM** |
| **Dim** (gate do TRIAC) | **GPIO5** | out | PSM: passa ou pula o semiciclo |
| SSR (resistência) | **GPIO6** | out | PID de temperatura |
| Livres | GPIO1, 2, 7, 15, 16, 17, 18, 21, 38–42, 47 | — | folga grande |

> GPIO4/GPIO5 eram a UART do DimmerLink. Com a
> [decisão de ir para PSM](#bypass-do-dimmerlink-para-psm-decidido) voltaram ao papel
> original de `Z-C`/`Dim`. **GPIO7 liberou** — era reserva do flow meter, que saiu do
> projeto ([04](04-bom-lista-de-compras.md)).

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

> ### 🔄 Decisão revista (29/07/2026): vamos de PSM, sem passar pelo DimmerLink
>
> Esta seção descreve o módulo **comprado** e continua válida como descrição de
> hardware — mas o **DimmerLink não será usado**. A bomba será atuada por **PSM**,
> com `Z-C` e `Dim` ligados direto ao ESP32. Ver
> [Bypass do DimmerLink para PSM](#bypass-do-dimmerlink-para-psm-decidido).
>
> Consequência: os itens 1 e 2 abaixo **se invertem**. Volta a existir pino de
> zero-cross e de gate, e volta a existir ISR de timing — bem menos crítica do que
> com ângulo de fase, mas existe.

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

### Protocolo (completo)

Não há biblioteca — são bytes crus. Todo comando começa com **STX = `0x02`**; sem
ele o comando é ignorado.

| Comando | Código | Formato | Descrição |
|---|---|---|---|
| **SET** | `0x53` | `02 53 IDX LEVEL` | define o nível |
| **GET** | `0x47` | `02 47 IDX` | lê o nível |
| **CURVE** | `0x43` | `02 43 IDX TYPE` | curva: 0=LINEAR, 1=RMS, 2=LOG |
| **GETCURVE** | `0x51` | `02 51 IDX` | lê a curva |
| **FREQ** | `0x52` | `02 52` | lê a frequência da rede |
| **RESET** | `0x58` | `02 58` | reset por software |
| **SWITCH_I2C** | `0x5B` | `02 5B` | muda para I²C |

Respostas: `0x00` OK · `0xF9` sintaxe · `0xFC` erro de EEPROM · `0xFD` índice
inválido · `0xFE` parâmetro inválido.

> 🔴 **`SWITCH_I2C` é irreversível pela UART.** Depois dele a UART é **desativada** e
> o estado fica gravado em EEPROM, sobrevivendo a reboot — só se fala com a placa por
> I²C em 0x50. Nunca emitir `0x02 0x5B` por acidente.

> ✅ **`SET` não grava em EEPROM** (só o modo de interface grava). Pode ser chamado no
> laço de controle dezenas de vezes por segundo sem desgastar a memória. Latência
> UART direta < 1 ms.

### ⚠️ Duas limitações que importam para este projeto

**1. Resolução de 0–100 inteiro.** O `LEVEL` é **1 byte, 0 a 100** — 101 degraus, e
nada mais fino. Não existe modo de atraso de fase cru nem upload de curva
personalizada. Para *pressure profiling* isso é grosseiro justamente onde mais dói:
a pré-infusão (3–4 bar) fica perto do mínimo da bomba, onde sobram poucos degraus
úteis. Atenuante: pré-infusão é **teto de pressão**, não setpoint perseguido — o que
se pede lá embaixo é ponto de operação estável, não autoridade fina de controle.
Ver [05](05-firmware-arquitetura.md) e [07](07-seguranca-e-calibracao.md).

**2. Só ângulo de fase.** Não há modo de pulos de ciclo (PSM). Ver
[bypass para PSM](#bypass-do-dimmerlink-para-psm-decidido) abaixo.

> Note que **mapeamento personalizado não é limitação**: o firmware monta a própria
> LUT `pressão desejada → comando 0–100` a partir da caracterização, e a curva do
> DimmerLink é só o que fica por baixo dela. O que **não** dá para consertar em
> software é a quantização em 101 degraus e a ausência de PSM.

### Curva de dimerização

Curvas **Linear**, **RMS** e **Logarítmica**, com auto-detecção de 50/60 Hz
(Brasil = 60 Hz, resolvido sozinho).

➡️ **Começar pela RMS.** "Linear" é linear no *ângulo de fase*, que não tem relação
útil com o que a bomba faz. A RMS mapeia o comando para **tensão RMS**, muito mais
próxima da grandeza que move um solenoide — lineariza o processo antes do PID e
melhora o aproveitamento dos 101 degraus. Confirmar na caracterização ([07](07-seguranca-e-calibracao.md)).

### A confirmar no módulo em mãos

- [ ] Quais pads estão expostos: UART, I²C, ou ambos.
- [ ] ⚠️ **Não** usar a biblioteca `RBDDimmer` — é do dimmer clássico, com pinos ZC/PWM.

## Bypass do DimmerLink para PSM (decidido)

**Este é o plano corrente**, desde 29/07/2026. Era Plano B condicional, à espera da
caracterização do DimmerLink; a decisão foi antecipar. A justificativa técnica já
estava toda escrita abaixo — o que mudou foi parar de tratá-la como hipótese.

> ### 🛒 Se for comprar de novo: o dimmer **clássico** é a peça certa
>
> O V2 comprado (com DimmerLink integrado) é a variante **errada** para PSM: paga-se
> por um controlador dedicado que será desligado, e chega-se aos sinais `Z-C`/`Dim`
> por **dessoldagem em placa de rede AC** — possivelmente trocando a ponte
> retificadora por um diodo.
>
> O **dimmer RobotDyn clássico expõe `Z-C` e `PWM` em header**, prontos. Custa
> ~R$ 32 — ordens de magnitude mais barato, e mais seguro, que cirurgia no V2.
>
> ➡️ Comprar o clássico e guardar o V2 como reserva. Só encostar o ferro no V2 se
> por algum motivo o clássico não vier.

### Como o Gaggiuino faz

O Gaggiuino **não** usa ângulo de fase na bomba: usa **PSM (Pulse Skip Modulation)**,
via a biblioteca [banoz/PSM.Library](https://github.com/banoz/PSM.Library). Em vez de
cortar o início de cada semiciclo, ele deixa passar **semiciclos inteiros** e decide
quais **pular**.

A diferença importa para bomba vibratória. Uma ULKA é um solenoide: cada semiciclo
puxa o pistão contra uma mola. Com ângulo de fase em potência baixa, o pulso chega
tarde e curto, a força não completa o curso e a bomba **zumbe sem bombear** — é
exatamente o sintoma que os usuários de Philco relatam ("o dimmer não desce abaixo de
11–12 bar"). Com PSM, todo pulso que chega é **inteiro e em força total**; controla-se
a vazão pela contagem de pulsos. O comportamento em baixa potência é muito melhor.

Bônus do PSM que perdemos com o DimmerLink: como cada pulso é um curso discreto da
bomba, dá para **estimar vazão contando pulsos + pressão, sem flow meter** — é assim
que o Gaggiuino faz *flow profiling*. Ver nota na [BOM](04-bom-lista-de-compras.md).

### ❌ Mito: "PSM trava e queima bomba vibratória"

Circula a afirmação de que PSM faria o pistão da Ulka perder o sincronismo com a
rede, chacoalhar, perder pressão e superaquecer a bobina — e que só ângulo de fase
seria seguro. **É o inverso do que acontece**, e vale registrar para não descartar o
plano B por engano.

- **Não há sincronismo a perder.** Bomba vibratória não é máquina síncrona: é um
  oscilador mola-massa-solenoide que recebe um impulso por ciclo. Sem rotor, sem
  momento angular. Pular um ciclo significa só que aquele curso não acontece — o
  pistão volta pela mola e espera. O pulso seguinte chega **inteiro** e completa o curso.
- **Quem produz curso incompleto é o ângulo de fase.** Pulso cortado e atrasado
  entrega energia parcial; em potência baixa o pistão não completa o percurso e a
  bomba zumbe sem bombear.
- **Aquecimento:** perda ∝ I²R × tempo ligado. PSM entrega pulsos de amplitude cheia
  numa fração do tempo e deixa a bobina esfriar entre eles; ângulo de fase dispara
  **todo** ciclo com onda picotada. O limite de duty cycle da Ulka (~1 min on /
  1 min off) é sobre tempo total de operação, não sobre método de modulação.
- **Evidência de campo:** o Gaggiuino roda PSM em bomba Ulka em milhares de
  instalações, e a `PSM.Library` existe exatamente para isso ("*PSM vibe pump control*").

**O tradeoff real do PSM — que existe — é outro: ripple de pressão.** Em duty baixo
os pulsos ficam espaçados e a pressão pulsa mais, o que em tese pode piorar
canalização. A complacência hidráulica e o próprio puck suavizam bastante, mas é este
o contra a pesar na decisão — não travamento nem bobina queimada. Relatos de campo
com bomba pulsada por microcontrolador confirmam o incômodo na forma de **mais ruído**,
não de dano.

> A única evidência que existe perto da alegação de desgaste é indireta e fraca: no
> [Home-Barista](https://www.home-barista.com/espresso-machines/successes-in-controlling-your-ulka-pump-for-pressure-profiling-t54317.html)
> menciona-se, de segunda mão, que o método patenteado da Breville de reduzir a saída
> da bomba encurtaria a vida dela. Só que a patente da Breville é **manipulação de
> forma de onda** — controle tipo fase, não PSM. Se esse relato valer algo, ele aponta
> para o método que estamos usando, não para o plano B.

### O caminho do bypass

A arquitetura do DimmerLink joga a favor: normalmente ele é uma placa **separada**, e
seu conector de saída expõe exatamente **VCC, GND, Z-C (zero-cross) e Dim (gate do
TRIAC)** — os mesmos dois sinais que o PSM precisa. Na variante integrada esses dois
estágios estão na mesma PCB, mas a interface entre eles continua existindo.

1. Localizar os sinais **Z-C** e **Dim** na placa (pads ou trilha entre os estágios).
2. Deixar o controlador DimmerLink em 0% (ou sem alimentação).
3. Ligar **Z-C → GPIO de interrupção** e **Dim → GPIO de saída** no ESP32.
4. Usar `banoz/PSM.Library`, como o Gaggiuino.

⚠️ **Não é bypass puro de fiação.** Os projetos de PSM sobre dimmer RobotDyn relatam
precisar **trocar a ponte retificadora do detector de zero-cross por um diodo**, para
que o ZC enxergue só o semiciclo negativo — é o que permite decidir se o próximo pulso
positivo será pulado. É dessoldagem em placa de rede AC. Verificar se a versão atual
da PSM.Library ainda exige isso antes de encostar o ferro.

**Custo de sair do DimmerLink:** volta a ISR de zero-cross para o firmware.

Mas **PSM é muito mais tolerante a jitter que ângulo de fase**, e isso merece ficar
explícito porque muda o tamanho do risco:

| | Precisão exigida do disparo |
|---|---|
| Ângulo de fase | dezenas de µs — o atraso **é** o nível de potência |
| **PSM** | centenas de µs — a decisão é binária (passa o semiciclo ou pula) |

Com PSM, jitter só apara levemente a borda de subida de um pulso que já ia passar
inteiro. Não desestabiliza o nível de potência, porque o nível vem da **contagem**
de pulsos, não do instante do disparo.

➡️ Requisito real: a ISR de zero-cross marcada **`ESP_INTR_FLAG_IRAM`**, para não
travar em cache miss de flash. Sem isso, o handler pode atrasar milissegundos
quando o cache estiver ocupado — e milissegundos, aí sim, custam semiciclo.

## Pinagem com placa de display RGB (se a interface virar placa integrada)

Placas tipo **Waveshare/SpotPear ESP32-S3-Touch-LCD-4.3** ou **GUITION JC8048W550**
não são displays — são ESP32-S3 completos que **substituem** o devkit. E display RGB
não usa SPI, o que resolve de graça a disputa de barramento com MAX31865 + microSD.

Em troca, o RGB come ~20 GPIOs (R3–R7, G2–G7, B3–B7, HSYNC, VSYNC, DE, PCLK) mais 3
do touch. Por isso essas placas trazem um **CH422G**, expansor de I/O por I²C.

⚠️ **`Z-C` e `Dim` não podem ir no expansor.** O CH422G é I²C: não gera interrupção,
e cada escrita custa centenas de µs. Os dois precisam ser **GPIO reais** do ESP32.

Na SpotPear sobra **GPIO6** (header de sensor) como único pino de fato livre. O
segundo tem que sair de canibalização:

| Candidato | Custo |
|---|---|
| GPIO15/16 (RS485) | passam pelo SP3485; conferir no esquemático se o lado do ESP32 é sangrável |
| GPIO19/20 (USB nativa / CAN) | perde a USB nativa — e é a porta que **grava**, ver [05](05-firmware-arquitetura.md#-o-dtr-da-usb-jtag-é-o-gpio0) |
| GPIO43/44 (console) | perde log serial |

O resto encaixa sem briga:

- **SSR → expansor, tranquilo.** É slow PWM de janela longa; latência de I²C é irrelevante.
- **ADS1115 → barramento I²C** existente, sem pino extra.
- **microSD** já está a bordo (GPIO11/12/13, CS no EXIO4).
- **CS do MAX31865** pode ir no expansor pagando latência por transação.

> Se essa placa entrar, **PSRAM vai a 120 MHz**: a doc da SpotPear diz que o
> benchmark do LVGL dá 41 FPS e que habilitar 120M é necessário. O
> `firmware/bringup/sdkconfig.defaults` está em `SPIRAM_SPEED_80M` e precisaria mudar.
>
> ⚠️ Divergência a confirmar antes de comprar: a wiki da SpotPear diz "8MB Flash",
> o anúncio diz **N16R8** e a Waveshare documenta **16MB**.

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
