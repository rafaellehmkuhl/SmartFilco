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
- Heartbeat na serial a cada 2 s (matiz, brilho, RGB, heap, core)

O JSON traz `ws` (assinantes conectados), `wstx` e `wserr`, que é como se depura
o push sem terminal serial.

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
