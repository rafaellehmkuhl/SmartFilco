# 03 — Esquema hidráulico

## Padrão adotado

- **Rosca:** 1/8" NPT em todo o circuito de pressão (casa com o manômetro/TEE já existentes na máquina).
- **Mangueira:** silicone/PTFE para alta temperatura, **4 mm**.
- ⚠️ Sensores de pressão baratos costumam vir em **G1/8 (BSP)**, que **não veda bem** com NPT. Comprar o sensor em 1/8" NPT **ou** incluir um **adaptador BSP→NPT**. Ver [BOM](04-bom-lista-de-compras.md).

## Circuito original (aproximado)

```
Reservatório → Bomba vibratória → (OPV?) → Boiler → Grupo → Porta-filtro
```

## Circuito modificado (alvo)

```
Reservatório
    │
    ▼
Bomba vibratória (AC, controlada por dimmer/TRIAC)
    │
    ▼
  TEE 1/8" NPT ──────► Manômetro analógico (indicação visual)
    │           └────► Sensor de pressão digital 0,5–4,5V / 0–13,8 bar → ADS1115
    │
    ▼
OPV ajustável ──► retorno ao reservatório (mangueira de alívio)
    │            (regular ~11–12 bar como teto; ver nota abaixo)
    ▼
Boiler ──► PT100 (superfície, 3 fios) → MAX31865 → PID
    │
    ▼
Grupo (= face do boiler + chuveiro; a Philco não tem grupo separado)
    │
    ▼
Porta-filtro
```

> **Faixa do sensor:** 200 PSI = **13,8 bar**, não os 16 bar previstos originalmente.
> Cobre o pico da bomba (~15 bar sem restrição, ~12–13 no porta-filtro) com margem
> suficiente e dá um pouco mais de resolução. Não há problema prático.

### Por que o sensor digital e o manômetro no mesmo ponto (TEE)

Ficam lendo **exatamente a mesma pressão**, o que permite calibrar o sensor
eletrônico contra o analógico e nunca perder a referência visual.

### Papel da OPV (Over-Pressure Valve)

- Devolve água ao reservatório quando a pressão passa do ajuste → limita a pressão máxima.
- Muitos usuários da Philco regulam em **~9 bar**.
- **Interação com o controle eletrônico da bomba:** se a OPV abrir cedo demais, ela "briga" com o *pressure profiling* (você não consegue subir acima do ponto da OPV). Estratégia recomendada: regular a OPV como **teto de segurança** (ex.: ~11–12 bar) e deixar o **perfil de pressão** por conta do controle da bomba em malha fechada. A decisão final depende da caracterização da bomba — ver [07](07-seguranca-e-calibracao.md).

## Instalação do sensor de temperatura

**Um sensor só**, na parede do boiler. A justificativa para ter cortado o segundo
está em [01](01-projeto-visao-geral.md#por-que-um-sensor-de-temperatura-e-não-dois);
o resumo é que a Philco não tem grupo — a caldeira parafusa direto no chuveiro, então
não há caminho térmico entre "boiler" e "grupo" para instrumentar.

- **PT100 de superfície, 3 fios**, com o melhor contato térmico possível: sob um dos
  parafusos existentes do boiler, com **pasta térmica**. É o sensor do PID.
- Preferir o ponto onde ficava o **termostato de extração original** (~95 °C) — é o
  ponto que o fabricante escolheu para representar a caldeira, e libera a fixação.
- Cabo afastado da fiação AC e do dimmer; o par vai direto ao MAX31865.

### A temperatura real da água

Não é medida em operação — é **caracterizada uma vez** com termopar K entre
porta-filtro e grupo e depois compensada por modelo no firmware. Procedimento em
[07](07-seguranca-e-calibracao.md#caracterização-do-offset-boiler-bolo).

## A verificar ao abrir a máquina (ver [06](06-engenharia-reversa-checklist.md))

- Se **já existe uma OPV** e qual a pressão de abertura.
- Onde exatamente está o TEE de pressão hoje e se há espaço para OPV + sensor + manômetro.
- Diâmetro/rosca reais das conexões (confirmar 1/8" NPT e mangueira 4mm).
- Comprimento de mangueira necessário e pontos de fixação.
- **Estado do parafuso do chuveiro** — ver [06](06-engenharia-reversa-checklist.md), é item zero.

## 🔴 Ausência de válvula solenoide — limita o perfil de pressão

A Philco **não tem válvula de 3 vias**. O corte de fluxo é feito por mola + pino de
silicone. Duas consequências, e a segunda é de projeto, não de acabamento.

**1. Ela sempre pinga** depois de desligar a extração. Afeta detecção de fim de shot
e logging por peso — o firmware não pode assumir corte seco.

**2. Descer a pressão é limitado pela hidráulica, não pelo controle da bomba.**
Numa máquina com solenoide de 3 vias, despressurizar é instantâneo: abre-se o
caminho para a bandeja. Aqui o **único** caminho de alívio durante o shot é através
do próprio puck. A constante de tempo é
`complacência do sistema ÷ condutância do puck`.

| O que se pede ao perfil | Viável? |
|---|---|
| Subir pressão (pré-infusão → rampa → 9 bar) | ✅ limitado só pela bomba |
| **Declínio gradual** ao longo de ~10 s | 🟡 provavelmente sim, é decaimento natural com a bomba aliviada |
| **Queda rápida / degrau para baixo** | ❌ não acontece, com nenhuma eletrônica |

Isso independe de DimmerLink, PSM ou qualquer método de controle — é a máquina.
Relato de campo consistente com isso: usuários de Ulka com dimmer conseguem variar
pressão **subindo**, mas descer é lento porque a pressão precisa sangrar pelo puck
([Home-Barista](https://www.home-barista.com/espresso-machines/successes-in-controlling-your-ulka-pump-for-pressure-profiling-t54317.html)).

➡️ O `ProfileModule` deve tratar a fase de declínio como **decaimento observado**, não
como setpoint a perseguir — senão o PID satura tentando algo hidraulicamente
impossível. Ver [05](05-firmware-arquitetura.md).

> **Único bypass manual existente:** abrir a válvula de vapor alivia pressão
> rapidamente. Automatizar isso exigiria acrescentar um solenoide — fora de escopo,
> mas é o caminho caso o declínio rápido vire requisito.
