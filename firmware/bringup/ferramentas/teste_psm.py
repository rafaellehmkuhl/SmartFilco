#!/usr/bin/env python3
"""Confere o acumulador de Bresenham do PSM, replicando a aritmética da ISR.

Por que testar cinco linhas de conta inteira: elas decidem quanta potência a bomba
recebe, rodam dentro de uma interrupção onde não dá para pôr `printf`, e erram de
um jeito que PARECE funcionar — duty 30% entregando 25% dá café ruim, não erro.

A ISR decide por CICLO COMPLETO (dois semiciclos), não por semiciclo: a Ulka
EP5/EX5 tem diodo em série e só uma polaridade produz curso. O teste modela a
bomba com diodo e confere o que importa — a vazão que sai — além da contagem.

Propriedades:

1. **Contagem** — em N ciclos, o número de ciclos liberados bate com o duty.
2. **Polaridade** — todo ciclo liberado leva os DOIS semiciclos. Com bomba de
   diodo, a vazão tem que acompanhar o duty em qualquer fase de partida. Foi este
   o bug da versão por semiciclo: 50% entregava 100% ou 0% conforme a fase.
3. **Distribuição** — 50% sai alternado por ciclo, sem agrupar.
4. **Extremos** — duty 0 nunca dispara e duty 1 sempre dispara.
"""

PSM_ESC = 1000


def isr(alvo, n_semi):
    """Mesma conta de `zc_isr` em hal_real.c. Devolve o gate em cada semiciclo."""
    acc = 0
    segunda = False
    gate = 0
    saida = []
    for _ in range(n_semi):
        if alvo <= 0:
            acc, segunda, gate = 0, False, 0
        else:
            segunda = not segunda
            if segunda:
                acc += alvo
                if acc >= PSM_ESC:
                    acc -= PSM_ESC
                    gate = 1
                else:
                    gate = 0
        saida.append(gate)
    return saida


def vazao_diodo(gates, fase):
    """Cursos de uma bomba com diodo: só semiciclos da polaridade `fase` contam."""
    return sum(g for i, g in enumerate(gates) if i % 2 == fase)


falhas = []
N = 2400  # semiciclos = 20 s de rede a 60 Hz = 1200 ciclos
CICLOS = N // 2

for pct in range(0, 101):
    g = isr(round(pct / 100 * PSM_ESC), N)
    # 1. contagem por ciclo
    ciclos = sum(g[0::2])
    if abs(ciclos - CICLOS * pct / 100) > 1:
        falhas.append(f"duty {pct}%: {ciclos} ciclos, esperado ~{CICLOS * pct / 100:.0f}")
    # cada ciclo é inteiro: o gate não muda no meio de um ciclo
    if g[0::2] != g[1::2]:
        falhas.append(f"duty {pct}%: ciclo partido entre semiciclos")
    # 2. polaridade: as duas fases possíveis da bomba entregam o mesmo
    for fase in (0, 1):
        v = vazao_diodo(g, fase)
        if abs(v - CICLOS * pct / 100) > 1:
            falhas.append(f"duty {pct}% fase {fase}: bomba de diodo deu {v} cursos, "
                          f"esperado ~{CICLOS * pct / 100:.0f}")

# 3. distribuição, em ciclos
for pct in (10, 25, 33, 50, 75, 90):
    c = isr(round(pct / 100 * PSM_ESC), N)[0::2]
    idx = [i for i, v in enumerate(c) if v]
    maior = max(b - a for a, b in zip(idx, idx[1:]))
    if maior > 100 / pct + 1:
        falhas.append(f"duty {pct}%: agrupou, maior intervalo {maior} ciclos")
if isr(500, 24)[0::2] != [1, 0] * 6 and isr(500, 24)[0::2] != [0, 1] * 6:
    falhas.append("50% não alternou por ciclo")

# 4. extremos
if any(isr(0, 500)):
    falhas.append("duty 0 disparou")
if not all(isr(PSM_ESC, 500)):
    falhas.append("duty 100% pulou semiciclo")
if sum(isr(10, N)[0::2]) != 12:
    falhas.append("duty 1%: esperado 12 ciclos em 20 s")

if falhas:
    print("✗ " + "\n✗ ".join(falhas[:20]))
    raise SystemExit(1)
print("✓ contagem por ciclo confere de 0 a 100%")
print("✓ todo ciclo liberado é inteiro — bomba com diodo recebe o duty em qualquer fase")
print("✓ distribuição sem agrupar; 50% alterna por ciclo")
print("✓ duty 0 e 100% sem meio-termo; 1% sem zona morta")
