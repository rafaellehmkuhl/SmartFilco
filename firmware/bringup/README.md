# bringup — prova de vida da placa

Primeiro firmware do projeto, em **ESP-IDF v5.5.5**. Não tem nada do domínio de
café: existe para validar placa, toolchain e o arranjo de cores antes que
qualquer PID entre em cena.

O que faz:

- **RGB (WS2812)** em passeio de matiz, saturação cheia, na task do **core 1**
- **Wi-Fi STA** + **`esp_http_server`** no **core 0**
- Página em `/` alimentada por **WebSocket** (`/ws`), push a 30 Hz
- **Controle de brilho** na página, de 0 (apagado) ao teto — pelo mesmo WebSocket
- `/rgb` devolve o mesmo JSON por HTTP — teste de fumaça curl-ável
- **Varredura I²C** a cada 2 s em GPIO 8/9, reportada na página e na serial
- **BME680** (temperatura, umidade, pressão, gás) e **VL53L1X** (distância), ao vivo
- **Nível de reservatório** desenhado a partir do ToF — 4 cm cheio, 25 cm vazio
- Heartbeat na serial a cada 2 s (matiz, brilho, RGB, heap, core)

O JSON traz `ws` (assinantes conectados), `wstx` e `wserr`, que é como se depura
o push sem terminal serial.

## Varredura I²C

Task no **core 0** varrendo `0x08`–`0x77` a cada 2 s com `i2c_master_probe()`. Fica
fora do core de tempo-real porque o probe bloqueia até 20 ms por endereço ausente.

Os pinos são **GPIO 8 (SDA) e 9 (SCL)** — os mesmos que a
[`docs/02`](../../docs/02-arquitetura-eletrica-pinout.md) reserva ao ADS1115.
Prototipar no pino definitivo evita refazer chicote depois.

**O log sai só quando a lista muda.** Na bancada isso vira "apareceu / sumiu" ao
plugar, em vez de encher a serial com a mesma lista a cada 2 s. Na página, o DOM
também só é recriado na mudança — 30 quadros por segundo não ficam remontando
elementos à toa.

Medido em 30/07/2026, com VL53L1X e BME280 ligados:

| Endereço | Dispositivo |
|---|---|
| `0x29` | VL53L1X |
| `0x77` | BME280 — ⚠️ não `0x76`: o `SDO` da placa roxa tem pull-up |

> `SDO` flutuante é **endereço instável**. Amarrar de propósito: GND → `0x76`,
> VCC → `0x77`.

O JSON traz `"i2c":[41,119]` em decimal; a página formata em hex e resolve o nome.

## Sensores

| Sensor | Endereço | Driver | Task |
|---|---|---|---|
| **VL53L1X** ToF | `0x29` | `grrtzm/vl53l1x_library` | junto com a varredura, 200 ms |
| **BME680** | `0x77` | `k0i05/esp_bme680` | **task própria**, 1 s |

Os dois componentes usam o **`i2c_master` novo** e aceitam um barramento já criado —
foi o critério de escolha. Alternativas populares (`espressif/bme280`,
`esp-idf-lib/bme680`, `francisduvivier/bme68x_sensorapi_espidf`) dependem de
`i2c_bus`/`i2cdev`, que usam o driver **legado** — e legado não divide porta com
`i2c_master` no IDF 5.x.

**Por que o BME680 tem task própria:** em modo forçado ele liga o aquecedor e
espera, bloqueando ~400 ms. No mesmo laço do ToF, isso viraria engasgo visível no
desenho do tanque.

### ⚠️ Duas armadilhas encontradas medindo

**O sensor não é BME280, é BME680.** `chip_id` lido = `0x61` (BME280 seria `0x60`,
BMP280 `0x58`). Mesmo endereço, mesmo encapsulamento, registradores e fórmulas de
compensação diferentes. Só apareceu porque o driver reporta o id em vez de falhar
calado — vale manter esse hábito.

**O componente documenta a pressão em hPa e devolve Pa.** Confiar no comentário do
header dá **100× a pressão real** (954 hPa vira 95423). Passou despercebido em
temperatura e umidade porque ali a escala do firmware e a da página se cancelavam.
Pegou porque sabíamos o valor esperado: ~945–955 hPa para os ~600 m de São José dos
Campos.

### Nível de água

O firmware manda **milímetros crus**; a conversão para nível mora na página, porque
é geometria do reservatório e não física do sensor:

```
nível% = (250 − mm) / (250 − 40) × 100,  limitado a 0..100
```

`status != 0` (medida fraca ou fora de alcance) **esmaece o tanque em vez de sumir
com o número** — tela em branco não diz se o sensor caiu ou se está sem alvo.

## Controle de brilho

O canal é o **WebSocket que já existia** — ele é bidirecional, e o handler já
precisava drenar frames de entrada para não travar o socket. Sem endpoint novo.

**Protocolo:** o cliente manda o brilho como **número decimal em texto** (`"0"` a
`"80"`). Não vale JSON para um inteiro.

```bash
# o estado vive na placa, não no navegador — recarregar a página não muda nada
curl -s http://<ip>/rgb | grep -o '"brilho":[0-9]*'
```

Três decisões que valem saber:

**O clamp é do servidor.** `brilho = clamp(atoi(frame), 0, MAXV)`. O cliente não
define o teto — mandar `999` resulta em `MAXV`, mandar `-5` resulta em `0`.

**Com brilho 0 o passeio de matiz continua rodando.** Ao reacender, a cor está no
ponto em que estaria, em vez de saltar de onde parou.

**O slider tem uma guarda de 800 ms.** O servidor manda o brilho 30×/s; sem isso
cada frame sobrescreveria o controle debaixo do dedo. A guarda solta o slider 800 ms
depois do último toque — cobre mouse, dedo e seta de teclado sem rastrear ponteiro,
e mantém a sincronia entre múltiplos clientes.

## Compilar e gravar

`main/wifi_cfg.h` guarda a credencial e **não é versionado**. Sem ele o build
falha — crie a partir do exemplo:

```bash
cp main/wifi_cfg.h.exemplo main/wifi_cfg.h
$EDITOR main/wifi_cfg.h

source ~/esp/esp-idf/export.sh
idf.py -C firmware/bringup build
idf.py -C firmware/bringup -p /dev/cu.usbmodem1101 flash monitor
```

⚠️ Gravar **pela USB-C nativa do S3**, não pela porta da ponte CH343 — aquela lê
mas não escreve. E se a placa não subir depois da gravação, aperte **RST**: o DTR
da USB-JTAG é o GPIO0 e derruba a placa em download mode. Detalhes em
[`docs/05`](../../docs/05-firmware-arquitetura.md#-o-dtr-da-usb-jtag-é-o-gpio0).

## Onde mexer

| Constante (`main/main.c`) | Efeito |
|---|---|
| `PINS` | GPIO do LED — 48 na v1.0 da DevKitC-1, 38 na v1.1. Aciona os dois |
| `MAXV` | brilho (V do HSV). No máximo o WS2812 ofusca e puxa ~60 mA |
| `TRANSICAO_MS` | duração de cada transição de matiz |
| `FRAME_MS` | período do quadro (20 ms = 50 fps) |
