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
  TEE 1/8" NPT ──────► Manômetro analógico 0–16 bar (indicação visual)
    │           └────► Sensor de pressão digital 0,5–4,5V / 0–16 bar → ADS1115
    │
    ▼
OPV ajustável ──► retorno ao reservatório (mangueira de alívio)
    │            (regular ~9–11 bar; ver nota abaixo)
    ▼
Boiler ──► PT1000 #1 (corpo do boiler) → MAX31865 → PID
    │
    ▼
Grupo ──► PT1000 #2 (saída do grupo, só monitoramento) → MAX31865
    │
    ▼
Porta-filtro
```

### Por que o sensor digital e o manômetro no mesmo ponto (TEE)

Ficam lendo **exatamente a mesma pressão**, o que permite calibrar o sensor
eletrônico contra o analógico e nunca perder a referência visual.

### Papel da OPV (Over-Pressure Valve)

- Devolve água ao reservatório quando a pressão passa do ajuste → limita a pressão máxima.
- Muitos usuários da Philco regulam em **~9 bar**.
- **Interação com o controle eletrônico da bomba:** se a OPV abrir cedo demais, ela "briga" com o *pressure profiling* (você não consegue subir acima do ponto da OPV). Estratégia recomendada: regular a OPV como **teto de segurança** (ex.: ~11–12 bar) e deixar o **perfil de pressão** por conta do controle da bomba em malha fechada. A decisão final depende da caracterização da bomba — ver [07](07-seguranca-e-calibracao.md).

## Instalação dos sensores de temperatura

- **PT1000 #1 (boiler):** melhor contato térmico possível com o corpo do boiler (rosca dedicada, ou fixada sob parafuso com pasta térmica). É o sensor do PID.
- **PT1000 #2 (grupo):** na saída do grupo / caminho da água até o porta-filtro. Não controla nada; serve para saber a temperatura **real** que chega ao café (o boiler pode marcar 95°C e a água sair a ~89°C). Ótimo para calibração/offset.

## A verificar ao abrir a máquina (ver [06](06-engenharia-reversa-checklist.md))

- Se **já existe uma OPV** e qual a pressão de abertura.
- Onde exatamente está o TEE de pressão hoje e se há espaço para OPV + sensor + manômetro.
- Diâmetro/rosca reais das conexões (confirmar 1/8" NPT e mangueira 4mm).
- Comprimento de mangueira necessário e pontos de fixação.
