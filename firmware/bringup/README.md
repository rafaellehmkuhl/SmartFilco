# bringup — prova de vida da placa

Primeiro firmware do projeto, em **ESP-IDF v5.5.5**. Não tem nada do domínio de
café: existe para validar placa, toolchain e o arranjo de cores antes que
qualquer PID entre em cena.

O que faz:

- **RGB (WS2812)** em passeio de matiz, saturação cheia, na task do **core 1**
- **Wi-Fi STA** + **`esp_http_server`** no **core 0**
- Página em `/` alimentada por **WebSocket** (`/ws`), push a 25 Hz
- `/rgb` devolve o mesmo JSON por HTTP — teste de fumaça curl-ável
- Heartbeat na serial a cada 2 s (matiz, RGB, heap, core)

O JSON traz `ws` (assinantes conectados), `wstx` e `wserr`, que é como se depura
o push sem terminal serial.

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
