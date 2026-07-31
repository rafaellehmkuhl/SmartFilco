"""Varre o macro timing do VL53L1X com o alvo parado e reporta a escala.

Hipótese sob teste: `VL53L1X_ULP_SetMacroTiming` escreve WORD em 0x005E/0x0061,
que são os registradores de período do VCSEL — e o período define quantos
milímetros vale cada contagem. Se a escala andar com o `mt`, está confirmado.

Uso:  python3 varre_tof.py <distância_real_cm>
"""

import json
import sys
import time
import urllib.request

HOST = "192.168.0.44"
REAL_CM = float(sys.argv[1]) if len(sys.argv) > 1 else 10.0
MTS = [8, 12, 16, 24, 32, 48, 64, 80, 100, 128, 160, 200, 255]
N = 12          # amostras por ponto
ASSENTA = 1.2   # s para o sensor produzir medida nova depois de trocar o mt


def le(mt=None):
    url = "http://%s/tof" % HOST + ("?mt=%d" % mt if mt is not None else "")
    return json.load(urllib.request.urlopen(url, timeout=4))


print("alvo declarado em %.1f cm\n" % REAL_CM)
print("  mt   mm(mediana)   faixa mm     escala   válidas  sigma  sinal")
print("  " + "-" * 62)

melhor = None
for mt in MTS:
    le(mt)
    time.sleep(ASSENTA)
    mm, ok, sg, sn = [], 0, [], []
    for _ in range(N):
        d = le()
        mm.append(d["mm"])
        sg.append(d["sg"])
        sn.append(d["sn"])
        if d["dst"] == 0:
            ok += 1
        time.sleep(0.22)
    mm.sort()
    med = mm[len(mm) // 2]
    esc = (med / 10.0) / REAL_CM
    print("  %3d   %6d       %4d-%-4d   %6.2f   %2d/%-2d   %4d  %6d"
          % (mt, med, mm[0], mm[-1], esc,
             ok, N, sorted(sg)[len(sg) // 2], sorted(sn)[len(sn) // 2]))
    # só conta como candidato se a medida for estável e não for ruído de fundo
    if mm[-1] - mm[0] < 60 and sorted(sn)[len(sn) // 2] > 300:
        if melhor is None or abs(esc - 1) < abs(melhor[1] - 1):
            melhor = (mt, esc)

print()
if melhor:
    print("mais perto de escala 1: mt=%d  (escala %.2f)" % melhor)
else:
    print("nenhum ponto estável — alvo pode não estar sendo visto")
