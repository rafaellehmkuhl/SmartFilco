#!/usr/bin/env python3
"""Confere o acumulador de Bresenham do PSM, replicando a aritmética da ISR.

Por que testar cinco linhas de conta inteira: elas decidem quanta potência a bomba
recebe, rodam dentro de uma interrupção onde não dá para pôr `printf`, e erram de
um jeito que PARECE funcionar — duty 30% entregando 25% dá café ruim, não erro.

As três propriedades que importam:

1. **Contagem** — em N semiciclos, o número de disparos tem que bater com o duty.
   Um erro de arredondamento aqui vira pressão sistematicamente errada.
2. **Distribuição** — 50% tem que sair alternado, não 60 disparos seguidos e 60 de
   silêncio. É a diferença entre bomba pulsando parelho e bomba engasgando; foi
   por isso que se escolheu Bresenham em vez de contador simples ([docs/02]).
3. **Extremos** — duty 0 nunca dispara e duty 1 sempre dispara, sem "quase".
"""

PSM_ESC = 1000


def isr(alvo, n):
    """Mesma conta de `zc_isr` em hal_real.c. Devolve a lista de semiciclos."""
    acc = 0
    saida = []
    for _ in range(n):
        if alvo <= 0:
            acc = 0
            saida.append(0)
            continue
        acc += alvo
        if acc >= PSM_ESC:
            acc -= PSM_ESC
            saida.append(1)
        else:
            saida.append(0)
    return saida


def maior_intervalo(s):
    """Maior distância entre disparos consecutivos — mede agrupamento."""
    idx = [i for i, v in enumerate(s) if v]
    if len(idx) < 2:
        return len(s)
    return max(b - a for a, b in zip(idx, idx[1:]))


falhas = []

# 1. contagem: 1200 semiciclos = 10 s de rede a 60 Hz
N = 1200
for pct in range(0, 101):
    s = isr(round(pct / 100 * PSM_ESC), N)
    esperado = N * pct / 100
    if abs(sum(s) - esperado) > 1:
        falhas.append(f"duty {pct}%: {sum(s)} disparos, esperado ~{esperado:.0f}")

# 2. distribuição
s50 = isr(500, 120)
if s50 != [0, 1] * 60:
    falhas.append(f"50% não alternou: {''.join(map(str, s50[:12]))}...")

for pct in (10, 25, 33, 50, 75, 90):
    s = isr(round(pct / 100 * PSM_ESC), 1200)
    # o ideal teórico é 100/pct; Bresenham não pode passar disso mais que 1
    teto = 100 / pct + 1
    g = maior_intervalo(s)
    if g > teto:
        falhas.append(f"duty {pct}%: agrupou, maior intervalo {g} > {teto:.1f}")

# 3. extremos
if any(isr(0, 500)):
    falhas.append("duty 0 disparou")
if not all(isr(PSM_ESC, 500)):
    falhas.append("duty 100% pulou semiciclo")

# 4. duty baixo continua entregando alguma coisa, e no ritmo certo.
#    Se 1% não disparar nunca, a bomba tem uma zona morta que o painel não mostra.
s1 = isr(10, 1200)
if sum(s1) != 12:
    falhas.append(f"duty 1%: {sum(s1)} disparos em 1200, esperado 12")

if falhas:
    print("✗ " + "\n✗ ".join(falhas))
    raise SystemExit(1)
print("✓ contagem confere de 0 a 100%")
print("✓ 50% alterna semiciclo a semiciclo, sem agrupar")
print("✓ duty 0 e 100% sem meio-termo")
print("✓ 1% entrega 12 disparos em 10 s — sem zona morta silenciosa")
