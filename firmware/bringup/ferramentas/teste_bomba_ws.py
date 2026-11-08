"""Cliente WS mínimo para o teste de bomba: manda comandos em instantes dados (ms)
e imprime o estado da bomba a cada 0,5 s. Ex.:
  python3 teste_bomba_ws.py '[[500,"bomba teste 0.4 2"]]'
Casos já conferidos no hardware (08/11/2026): fim por tempo, PARAR manual, teto
de 10 s num pedido de 60 s, duty NaN virando 0."""
import socket, os, base64, json, time, sys, struct
H = os.environ.get("HOST", "smartfilco.local")
passos = json.loads(sys.argv[1]); dur = max(p[0] for p in passos) / 1000 + 3
s = socket.create_connection((H, 80), 5)
k = base64.b64encode(os.urandom(16)).decode()
s.send(f"GET /ws HTTP/1.1\r\nHost: {H}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
       f"Sec-WebSocket-Key: {k}\r\nSec-WebSocket-Version: 13\r\n\r\n".encode())
buf = b""
while b"\r\n\r\n" not in buf: buf += s.recv(1024)
buf = buf.split(b"\r\n\r\n", 1)[1]
def env(txt):
    d = txt.encode(); m = os.urandom(4)
    s.send(bytes([0x81, 0x80 | len(d)]) + m + bytes(b ^ m[i % 4] for i, b in enumerate(d)))
def frame():
    global buf
    while True:
        if len(buf) >= 2:
            n = buf[1] & 0x7f; o = 2
            if n == 126:
                if len(buf) < 4: pass
                else: n = struct.unpack(">H", buf[2:4])[0]; o = 4
            if (buf[1] & 0x7f) != 126 or o == 4:
                if len(buf) >= o + n:
                    p = buf[o:o+n]; buf = buf[o+n:]; return p
        buf += s.recv(4096)
t0 = time.time(); ult = -1; pend = sorted(passos)
while time.time() - t0 < dur:
    t = (time.time() - t0) * 1000
    while pend and pend[0][0] <= t:
        print(">>", pend[0][1]); env(pend.pop(0)[1])
    try: m = json.loads(frame())["m"]
    except Exception: continue
    if t - ult >= 500:
        ult = t
        print(f"{t/1000:5.1f}s est={m['est']} bmb={m['bmb']:.2f} psm={m['psm']:.2f} bt={m['bt']} btf={m['btf']!r}")
