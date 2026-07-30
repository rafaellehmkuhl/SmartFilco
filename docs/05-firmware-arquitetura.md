# 05 — Arquitetura de firmware

> Este documento fixa a **arquitetura alvo**. A implementação começou:
> [`firmware/bringup/`](../firmware/bringup/) é o primeiro código no repo — prova
> de vida da placa em ESP-IDF (RGB + Wi-Fi + servidor web), sem nada do domínio
> de café ainda.

## Princípios

- **Modular**, orientado a responsabilidades. Cada periférico/serviço é um módulo isolado.
- Separação lógica entre **tempo-real/segurança** (leitura de sensores, PID, proteções)
  e **interface** (Wi-Fi, API, depois LVGL) — mesmo rodando num único ESP32-S3.
- **Segurança em primeiro lugar:** proteções por software independentes da UI.
- **Headless primeiro.** A primeira fase não tem display: a interface é web/serial.
  Isso é uma vantagem arquitetural — obriga a API a ser a fonte única de verdade, e
  o LVGL entra depois como **mais um cliente** dela, não como caminho paralelo.

## Divisão em duas "placas lógicas"

Embora fisicamente seja um único ESP32:

- **Control Board (tempo-real):** sensores, PID de temperatura, controle de pressão, atuadores, watchdog, limites de segurança.
- **UI Board (interface):** Wi-Fi, web server, API, logging — e, na fase de interface, o touchscreen (LVGL).

Na prática, sugestão de uso dos dois núcleos do ESP32-S3:
- **Core 0:** loop de controle determinístico (amostragem de sensores, PID, disparo da bomba, SSR, proteções).
- **Core 1:** Wi-Fi, web, gravação no microSD — e depois UI/LVGL.

## Módulos propostos

| Módulo | Responsabilidade | Fase |
|---|---|---|
| `TemperatureModule` | Lê o MAX31865 (SPI), checa o registrador de **fault**, aplica filtro, expõe `T_boiler` e `T_bolo_estimada` (offset modelado) | 1 |
| `PressureModule` | Lê o sensor via ADS1115 (I²C), converte 0,5–4,5 V → bar, **filtra a ondulação de 60 Hz da bomba**, aplica calibração | 1 |
| `PumpModule` | Controle da bomba por **PSM**: ISR de zero-cross (`Z-C`, em **IRAM**) decide passar ou pular cada semiciclo no `Dim`. Base: [`banoz/PSM.Library`](https://github.com/banoz/PSM.Library). LUT `pressão → duty` + PID de pressão (malha fechada) | 1 |
| `BoilerModule` | PID de temperatura → SSR (slow PWM por janela de tempo) | 1 |
| `SafetyModule` | Limites de temperatura/pressão, timeout de bomba, sensor aberto/curto/travado, watchdog, estado seguro no boot | 1 |
| `LoggerModule` | Gravação de cada shot no microSD (CSV/JSON) + histórico | 1 |
| `WiFiModule` | Conexão à rede local, mDNS, **NTP** (substitui o RTC) | 1 |
| `WebServer` | UI web + API REST/WebSocket para gráficos em tempo real — **interface principal da fase 1** | 1 |
| `Storage` | Configurações persistentes (NVS + microSD) | 1 |
| `ShotModule` | Máquina de estados da extração: pré-infusão → ramp → hold → declínio; timer, integra fluxo (futuro) | 2 |
| `ProfileModule` | Perfis de pressão/temperatura, armazenados no microSD. Pré-infusão como **teto**, declínio como **decaimento observado** — ver abaixo | 2 |
| `SteamModule` | Modo vapor (setpoint alto, teto pelo termostato de vapor em hardware) | 2 |
| `OTA` | Atualização de firmware pela rede | 2 |
| `DisplayModule` | UI LVGL — **cliente da mesma API do WebServer** | 3 |

> **`RTCModule` foi removido.** Sem o board Waveshare não há PCF85063; o timestamp
> de shot vem de **NTP** via Wi-Fi. Só voltaria a fazer sentido se houvesse
> requisito de log correto sem rede — não há.

## Loop de controle (essência)

```
a cada ~50–100 ms:
  T_boiler = TemperatureModule.readBoiler()
  P        = PressureModule.readBar()

  # temperatura
  duty_ssr = BoilerModule.pid(setpoint_T, T_boiler)
  SSR.apply(duty_ssr)                 # janela de tempo (slow PWM)

  # pressão (durante o shot)
  if shot.active:
    p_target = ProfileModule.targetAt(shot.elapsed)
    duty_pump = PumpModule.pid(p_target, P)   # malha fechada
    PumpModule.setDuty(duty_pump)              # só grava o duty; quem dispara é a
                                               # ISR de zero-cross, assíncrona

  SafetyModule.check(T_boiler, P, currents)
```

> Note que `T_group` **saiu do laço** — existe um sensor só. A temperatura da água
> no bolo é derivada por modelo de offset, não medida em operação
> ([07](07-seguranca-e-calibracao.md#caracterização-do-offset-boiler-bolo)).

## Interface

### Fase 1 — web (headless)

Página única servida pelo ESP32, WebSocket para telemetria ao vivo:

```
   93.5 °C  (bolo ≈ 89.2 °C est.)      8.9 bar
   ┌────────────────────────────────────────┐
   │  gráfico ao vivo P / T                 │
   └────────────────────────────────────────┘
   Shot 00:18                    histórico ▾
```

Controles: **Coffee · Steam · Flush · Settings** + edição de setpoint e perfil.

### Fase 3 — touch local

Mesma informação em LVGL num 3,5", consumindo a **mesma API**. Nenhuma lógica de
controle vive na camada de UI, em nenhuma das duas.

## Dados registrados por shot

`tempo, T_boiler, T_bolo_estimada, pressão, potência_resistência, potência_bomba, fluxo(futuro)`
→ gravados no microSD, visualizáveis em gráfico no navegador.

> Guardar sempre `T_boiler` **crua**, além da estimada. É o que permite refinar o
> modelo de offset depois, em cima do histórico, sem repetir a bancada.

## Stack — decidida

**ESP-IDF v5.5.5.** Decisão tomada em 29/07/2026, no bring-up da Leva 1.

O ponto que costuma passar despercebido: **Arduino-ESP32 3.x *é* ESP-IDF** — mesmo
FreeRTOS, mesmo LWIP, mesmo driver de Wi-Fi, com APIs por cima. Não existe
interpretador nem overhead de runtime, e **performance bruta é idêntica**. A
diferença é *controle sobre a configuração*:

| | ESP-IDF | Arduino-ESP32 |
|---|---|---|
| `sdkconfig` | menuconfig completo | libs pré-compiladas, config congelada |
| Core do Wi-Fi/LWIP | fixado por config | herda o padrão |
| Tick do FreeRTOS | escolhido (padrão 100 Hz) | fixo em 1000 Hz |
| LVGL | port oficial + `esp_lcd` com DMA | caminho comum (TFT_eSPI) é pior |
| Web | `esp_http_server`, WebSocket nativo | `WebServer.h` ou ESPAsyncWebServer |
| Debug | JTAG pela USB nativa, gdb, heap tracing | funciona, mal integrado |

O argumento decisivo não foi performance: é que a fase 3 traz LVGL consumindo a
mesma API. Começar no Arduino significaria migrar depois, no pior momento.

> **As malhas de controle são o requisito menos exigente dos três.** O laço de
> 50-100 ms é folgado para qualquer um dos dois. O risco real não é throughput,
> é **jitter do Wi-Fi disputando o mesmo core** — e é isso que o IDF fixa por
> configuração. O gargalo de verdade do projeto é o LVGL, que depende de buffer
> DMA em SRAM interna e barramento SPI dedicado.

- Web: `esp_http_server` + WebSocket para telemetria; front embarcado no flash
  via `EMBED_TXTFILES`. Já em pé no bring-up: push a 25 Hz, medido em 24,8 Hz
  com mediana de 41 ms entre frames.

> ⚠️ **Duas armadilhas do WebSocket no esp_http_server** (custaram tempo no
> bring-up, valem para o `WebServer` da fase 1):
>
> 1. `CONFIG_HTTPD_WS_SUPPORT` vem **desligado** por padrão.
> 2. **O handler não é chamado no handshake.** Está no `httpd_uri.c`: *"If the
>    request is websocket handshake, then do not call the uri->handler"*. O padrão
>    antigo de registrar o socket dentro de `if (req->method == HTTP_GET)` não
>    dispara — o cliente conecta, recebe 101, e nunca é registrado. Sintoma:
>    handshake perfeito e zero frames.
>
> ➡️ Não manter lista própria de clientes. `httpd_get_client_list()` +
> `httpd_ws_get_fd_info()` dizem quem está conectado e quem falou WS. Isso apaga
> registro, slots e limpeza de socket morto, e nunca dessincroniza.
- **LVGL** só na fase 3 (ST7796 + FT6336). ⚠️ O SPI já é compartilhado entre
  MAX31865 e microSD — o display precisa do **outro host SPI**, sozinho, ou a
  fluidez morre.

### Ajustes de `sdkconfig` que valem

São exatamente os que o Arduino não deixaria mexer. Ver
[`firmware/bringup/sdkconfig.defaults`](../firmware/bringup/sdkconfig.defaults):

- `CONFIG_FREERTOS_HZ=1000` — o padrão do IDF é 100 Hz, passo de 10 ms, grosso
  demais para o laço de pressão.
- `CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0` + `CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0`
  — mantém a rede longe do core 1.
- `CONFIG_SPIRAM_MODE_OCT` + `SPEED_80M` — a PSRAM do N16R8.
- `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG` — console pela USB nativa.

> Símbolo desconhecido em `sdkconfig.defaults` é **ignorado em silêncio** pelo
> IDF. Escrever `CONFIG_ESP_WIFI_TASK_CORE_ID_0` (que não existe) não dá erro
> nenhum — só não faz nada. Conferir contra o Kconfig depois de editar.

## Como modelar as fases do shot nesta máquina

As três fases **não** são todas "um setpoint que o PID persegue". Tratar as três do
mesmo jeito produz saturação e *windup* em duas delas.

### Pré-infusão = teto de pressão, não setpoint

Com o puck seco a resistência é baixa: a água entra fácil e a pressão fica baixa
**sozinha**. Conforme o bolo satura e incha, a resistência sobe e a pressão sobe
junto. O valor observado no fim da fase é aproximadamente a **pressão de equilíbrio
da bomba naquela potência** contra um puck já molhado.

Não é à toa que no Gaggiuino o parâmetro é literalmente *"max reachable pressure for
the pre-infusion phase"*. Implementar como **potência baixa estável + limitador**, não
como PID perseguindo 3 bar num sistema que ainda não consegue gerar 3 bar.

🔄 **Com PSM esta fase deixa de ser o problema que era.** O argumento original dizia
que a resolução grosseira do DimmerLink incomodava pouco aqui, porque o que se pede
lá embaixo é um ponto de operação estável. Verdade — mas a raiz do problema era
outra: **em potência baixa o ângulo de fase entrega curso incompleto**, e a bomba
zumbe sem bombear. PSM entrega pulso inteiro sempre, só mais espaçado. Pré-infusão a
3–4 bar sai naturalmente, e a faixa de 2 bar (*blooming*), que estava provavelmente
fora de alcance, volta a ser plausível.

**Faixa alvo: 3–4 bar.** É a faixa mais usada na prática (a referência canônica de
~3 bar vem da *line pressure* das máquinas comerciais plumbadas). Perfis de *blooming*
a 2 bar provavelmente estão fora de alcance com ângulo de fase — ver [07](07-seguranca-e-calibracao.md).

### Rampa e hold = PID de verdade

É aqui que a malha fechada trabalha. Nenhuma ressalva.

### Declínio = decaimento observado, não perseguido

A Philco não tem solenoide de 3 vias: a pressão só cai sangrando pelo puck
([03](03-esquema-hidraulico.md)). Declínio gradual ao longo de ~10 s funciona;
degrau para baixo não acontece com nenhuma eletrônica.

➡️ Implementar como **redução programada da potência da bomba**, registrando a
pressão resultante — e **não** como setpoint decrescente com PID. Se for setpoint, o
integrador vai acumular erro contra um limite físico e a saída vai grudar no fundo.
Se o perfil declinante for setpoint mesmo assim, prever *anti-windup* e limite de
taxa compatível com a constante de tempo real, medida na caracterização.

## Cuidados específicos deste hardware

- **SPI compartilhado** entre MAX31865 (modo 1) e microSD (modo 0): todo acesso
  dentro de `beginTransaction` com o `SPISettings` correto por dispositivo. Acesso
  solto fora de transação é o bug clássico aqui.
- **Filtro na pressão** é obrigatório, não opcional: a bomba vibratória pulsa a
  60 Hz e sem filtro o PID de pressão persegue a ondulação.
- **Fault do MAX31865:** ler o registrador de falha, não inferir por valor absurdo.
- **Estado seguro no boot:** SSR e bomba desligados **antes** de qualquer
  inicialização de Wi-Fi ou UI. Com PSM isso é mais simples que com o DimmerLink:
  não há controlador remoto guardando o último valor — parar de disparar o `Dim`
  **é** parar a bomba. O estado inseguro no boot seria um GPIO flutuando, então
  configure `Dim` como saída em nível baixo antes de habilitar a ISR.
- **A ISR de zero-cross vai em IRAM.** `ESP_INTR_FLAG_IRAM`, e nada dentro dela
  pode tocar flash (sem `ESP_LOGx`, sem chamada não-IRAM). Um cache miss de flash
  atrasa o handler em milissegundos, e milissegundos custam semiciclo.
- **PSM tolera jitter; ângulo de fase não.** Com ângulo de fase o instante do
  disparo **é** o nível de potência, e erro de dezenas de µs já desestabiliza. Com
  PSM a decisão é binária — passa o semiciclo ou pula — e o nível vem da
  **contagem**. Jitter de centenas de µs só apara a borda de subida.
- **Watchdog da bomba continua obrigatório**, por outro motivo: um ESP32 travado com
  a ISR ainda disparando mantém a bomba ligada. O `SafetyModule` precisa de um
  caminho que force `Dim` baixo em qualquer falha.
- **Nada de `RBDDimmer`.** Usar [`banoz/PSM.Library`](https://github.com/banoz/PSM.Library),
  que é a implementação de PSM para bomba vibratória usada pelo Gaggiuino.
- **Quantização agora é o número de semiciclos**, não 101 degraus. A 60 Hz são 120
  semiciclos/s: uma janela de 1 s dá resolução de ~0,8%, melhor que o DimmerLink. Mas
  a janela também é o tempo de resposta — janela curta responde rápido e quantiza
  grosso. Escolher com o dado da caracterização, e prever zona morta contra
  *limit cycle*.
- **`PumpModule` atrás de uma interface.** Continua valendo, agora no sentido
  inverso: se o PSM decepcionar, voltar ao DimmerLink (que está comprado) deve ser
  uma implementação nova do mesmo contrato — não uma refatoração do `ShotModule`.

## Gravação do firmware (esptool)

Módulo em mãos, identificado na bancada: **ESP32-S3 QFN56, revisão v0.2, PSRAM
embutida 8 MB (AP_3v3), cristal 40 MHz, MAC `74:4d:bd:8a:9d:d8`**.

A placa **não** usa o USB nativo do S3 (VID Espressif `303A`) — ela enumera atrás
de uma ponte **WCH CH343** (`1A86:55D3`), como `/dev/cu.usbmodem57280307871` no
macOS. O sufixo vem do número de série do CH343, então o nome da porta é estável.

⚠️ **Pela porta UART (CH343) só dá para ler — toda escrita falha.** Medido na
bancada, esptool v4.8.1:

| Operação | Resultado |
|---|---|
| `chip_id`, `flash_id`, `get_security_info` | ✅ funcionam |
| `read_flash` (com `--no-stub`) | ✅ funciona, ~7 kB/s |
| Upload do stub (escrita em RAM) | ❌ `Failed to write to target RAM ... Operation timed out` |
| `write_flash` (com `--no-stub`) | ❌ `Failed to write to target Flash after seq 0 ... Requested resource not found` |
| `erase_flash` (com `--no-stub`) | ❌ ROM do S3 não implementa a função |

O padrão é claro: comandos curtos e leitura passam, **transferência em bloco não**.
Não é baud (falha igual em 115200 e 460800), não é compressão (`-u` falha igual),
e não é proteção — secure boot e flash encryption estão **desligados**
(verificado por `get_security_info`).

➡️ **Gravar pela outra porta USB-C.** A DevKitC-1 tem duas: a serial via CH343 e a
**USB nativa do S3** (USB-OTG/JTAG, VID `303A`). A nativa não passa pela ponte WCH
e é o caminho suportado pela Espressif para flash. A porta UART continua ótima para
monitor serial e leitura.

Se a nativa também falhar na escrita, aí o suspeito é cabo/alimentação — troque o
cabo por um de dados comprovado e ligue direto na máquina, sem hub.

Pela nativa a gravação é ~180× mais rápida: **1293 kbit/s** contra os ~7 kB/s da
CH343, com `erase_flash` em 5,5 s.

### ⚠️ O DTR da USB-JTAG é o GPIO0

Esta foi a armadilha mais cara do bring-up. No USB-Serial-JTAG do S3:

| Linha CDC | Pino |
|---|---|
| RTS | EN (reset) |
| **DTR** | **GPIO0 (strapping de boot)** |

Ou seja: **qualquer coisa que abra aquela porta e assere DTR joga a placa em
download mode**, onde ela não roda o app e não imprime nada. Perdi um bom tempo
achando que o firmware estava quebrado — ele estava gravado e correto desde o
início, e a placa é que nunca saía do bootloader.

Sintomas que confundem:

- `esptool ... --after hard_reset` termina dizendo "Hard resetting", mas o boot
  seguinte reporta `boot:0x0 (DOWNLOAD(USB/UART0))`.
- Abrir a porta com `pyserial` — mesmo com `dtr=False` atribuído antes do `open()`
  — não foi suficiente no macOS; o driver CDC assere por conta.
- **LED aceso não prova que o firmware roda.** WS2812 retém a última cor
  latcheada indefinidamente. Dois quadros de vídeo idênticos foram lidos como
  "animação lenta" quando na verdade a cor estava congelada de uma execução
  anterior.

➡️ **Diagnóstico de um comando:** se `esptool --before no_reset` conectar, a placa
está parada no bootloader.

➡️ **Saída:** apertar **RST** fisicamente. Nenhuma sequência de DTR/RTS pelo host
resolveu de forma confiável.

### Fluxo de trabalho que funciona

```bash
source ~/esp/esp-idf/export.sh
idf.py -C firmware/bringup build
idf.py -C firmware/bringup -p /dev/cu.usbmodem1101 flash monitor
```

`idf.py monitor` faz a sequência de linhas na ordem certa — mas **exige TTY**, não
roda em pipe nem em CI. Para captura não-interativa não há solução boa: o jeito é
o firmware falar sozinho. Por isso o `bringup.c` tem heartbeat de 2 s imprimindo
matiz, RGB, heap e core — sem ele a placa é indepurável sem terminal.

### Dados de flash confirmados

Flash **GigaDevice `C8:4018`, 16 MB, modo quad**, tensão 3,3 V por eFuse — bate com
o N16R8. Firmware que veio de fábrica era o demo `arduino-lib-builder` (esp-idf
v4.4.7, Mar/2024), layout de partições Arduino (`otadata`/`app0`/`spiffs`/`coredump`).
