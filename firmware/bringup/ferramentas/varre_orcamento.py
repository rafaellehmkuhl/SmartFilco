"""Varre o orçamento de tempo do VL53L1X e mostra a troca estabilidade x taxa.

O que se está comprando com integração mais longa é REJEIÇÃO DE LUZ AMBIENTE. Na
bancada o ambiente chegou a 848 kcps contra 352 de sinal — o sensor recebendo
2,4x mais IV da sala do que do próprio alvo. Isso enviesa a distância, e como o
sinal do alvo cai com o quadrado da distância enquanto o ambiente é constante, o
viés cresce com a distância e se disfarça de erro de escala.

A coluna que decide é `sinal/amb`: abaixo de 1 o sensor está ouvindo mais a sala
que o eco. Distância só vale ser olhada quando essa razão passa de 1.

Uso:  python3 varre_orcamento.py [distância_real_cm]
      (alvo plano PARADO — apoiado, não na mão)
"""

import json
import sys
import time
import urllib.request

HOST = "smartfilco.local"
REAL_CM = float(sys.argv[1]) if len(sys.argv) > 1 else 0.0
# valores que a ULD aceita; 500 fica de fora, taxa cairia a ~2 Hz
ORCAMENTOS = [15, 20, 33, 50, 100, 200]
JANELA = 4.0     # s de coleta por ponto


def get(q=""):
    return json.load(urllib.request.urlopen("http://%s/tof%s" % (HOST, q), timeout=6))


print("alvo real: %s\n" % ("%.1f cm" % REAL_CM if REAL_CM else "não declarado"))
print("  tb    Hz    mediana   faixa cm      sigma  sinal   amb   sinal/amb  válidas")
print("  " + "-" * 74)

for tb in ORCAMENTOS:
    r = get("?tb=%d" % tb)
    if r.get("acao") == "falhou":
        print("  %3d   -- recusado pela ULD --" % tb)
        continue
    time.sleep(1.0)                      # deixa assentar na nova configuração

    amostras, vistos = [], set()
    t_ini = time.time()
    n0 = get()["tn"]
    while time.time() - t_ini < JANELA:
        d = get()
        if d["tn"] not in vistos:        # só medida nova; o HTTP é mais rápido que o sensor
            vistos.add(d["tn"])
            amostras.append(d)
        time.sleep(0.02)
    hz = len(vistos) / JANELA

    if not amostras:
        print("  %3d   sem amostra" % tb)
        continue
    mm = sorted(a["mm"] for a in amostras)
    med = mm[len(mm) // 2]
    sg = sum(a["sg"] for a in amostras) / len(amostras)
    sn = sum(a["sn"] for a in amostras) / len(amostras)
    am = sum(a["amb"] for a in amostras) / len(amostras)
    ok = sum(1 for a in amostras if a["dst"] == 0)
    razao = sn / am if am else float("inf")

    print("  %3d  %4.1f   %5.1f cm  %5.1f-%-5.1f  %5.0f  %5.0f  %5.0f   %6.2f    %2d/%-2d"
          % (tb, hz, med / 10.0, mm[0] / 10.0, mm[-1] / 10.0,
             sg, sn, am, razao, ok, len(amostras)))

print()
print("  sinal/amb < 1  ->  o sensor ouve mais a sala que o próprio eco")
if REAL_CM:
    print("  compare a mediana com %.1f cm para ver o viés sobrando" % REAL_CM)
