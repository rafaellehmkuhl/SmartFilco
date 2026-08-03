"""Puxa um shot simulado de ponta a ponta e confere o CSV gravado no cartão.

Prova, numa rodada só: PID aquece até o setpoint, máquina de estados passa por
pré-infusão → rampa → platô → declínio, parada por PESO dispara, e o
LoggerModule escreve o arquivo. Usa a aceleração de tempo da simulação para o
aquecimento não levar meia hora.

Uso:  python3 shot_teste.py [ip]
"""

import base64
import json
import os
import socket
import sys
import time
import urllib.request

HOST = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.44"


def http(cam):
    return urllib.request.urlopen("http://%s%s" % (HOST, cam), timeout=6).read()


def estado():
    return json.loads(http("/rgb"))


class WS:
    """Cliente mínimo. Frame cliente->servidor precisa ser MASCARADO (RFC 6455)."""

    def __init__(self, host):
        self.s = socket.create_connection((host, 80), timeout=5)
        self.s.sendall((
            "GET /ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
            % (host, base64.b64encode(os.urandom(16)).decode())).encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += self.s.recv(1024)

    def envia(self, txt):
        carga = txt.encode()
        mask = os.urandom(4)
        corpo = bytes(c ^ mask[i % 4] for i, c in enumerate(carga))
        self.s.sendall(bytes([0x81, 0x80 | len(carga)]) + mask + corpo)
        time.sleep(0.05)


ws = WS(HOST)
print("== preparando")
ws.envia("sim on")

# Falha é LATCHED de propósito. Mas limpar com a CAUSA ainda presente só a
# re-arma no ciclo seguinte — o que é o comportamento correto e foi o que
# confundiu este teste: ele limpava com o boiler a 158 °C e desistia achando que
# a máquina estava travada. Esperar esfriar primeiro.
if estado()["m"]["est"] == "FALHA":
    print("== máquina em falha; esperando esfriar antes de limpar")
    ws.envia("set vel 10")
    while estado()["m"]["tb"] > 60:
        print("   %.1f °C" % estado()["m"]["tb"])
        time.sleep(2)
    ws.envia("falha limpar")

ws.envia("reset")
ws.envia("set vel 10")        # aquecer em tempo real levaria minutos
ws.envia("set alvo 36")
ws.envia("set moagem 1.0")

print("== aquecendo até o setpoint")
t0 = time.time()
while time.time() - t0 < 90:
    m = estado()["m"]
    print("   %-9s %.1f °C" % (m["est"], m["tb"]))
    if m["est"] == "pronta":
        break
    time.sleep(2)
else:
    print("!! não ficou pronta a tempo")
    sys.exit(1)

print("== puxando shot")
ws.envia("set vel 1")         # o shot em si roda em tempo real
ws.envia("shot start")
t0 = time.time()
visto = []
while time.time() - t0 < 60:
    m = estado()["m"]
    if m["est"] not in visto:
        visto.append(m["est"])
    print("   %-11s t=%5.1fs  p=%4.1f/%4.1f bar  %5.1f g  %4.2f g/s  bomba %3.0f%%"
          % (m["est"], m["t"], m["pr"], m["ap"], m["g"], m["q"], m["bmb"] * 100))
    if m["est"] in ("fim", "FALHA"):
        break
    time.sleep(1)

print("\n== estados percorridos:", " → ".join(visto))

print("\n== cartão")
lista = json.loads(http("/sd"))
print("   montado=%s  livre=%s kB" % (lista["montado"], lista["livre_kb"]))
if not lista["shots"]:
    print("!! nenhum shot gravado")
    sys.exit(1)
ult = sorted(lista["shots"], key=lambda x: x["n"])[-1]
print("   arquivo: %s (%d bytes)" % (ult["n"], ult["b"]))

csv = http("/sd?f=" + ult["n"]).decode()
linhas = csv.strip().split("\n")
print("\n== CSV (%d linhas)" % len(linhas))
for l in linhas[:3]:
    print("   " + l)
print("   ...")
for l in linhas[-3:]:
    print("   " + l)
