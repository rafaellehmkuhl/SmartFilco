# 01 — Visão geral do projeto

## Objetivo

Digitalizar e automatizar por completo uma cafeteira espresso **Philco 15 Bar**,
substituindo o controle mecânico original por um sistema baseado em **ESP32-S3**
com:

- Medição **digital** de temperatura e pressão
- Controle **digital** de temperatura (PID) e de pressão (malha fechada)
- **Wi-Fi / web app** na rede local (interface principal na primeira fase)
- **Logging** de cada extração (shot) e visualização em gráficos
- **Display touch** local — *adiado para a fase de interface*
- Recursos avançados: perfis de pré-infusão e *pressure profiling*, flush, backflush, steam, OTA

A meta não é só "modificar", é produzir um **manual de engenharia reproduzível**
(esquemas, BOM, firmware, documentação) — algo acima dos mods existentes da Philco,
que hoje usam PID comerciais (Inkbird, REX-C100, XMT7100) e nenhum microcontrolador moderno.

## A máquina

- **Modelo:** Philco 15 Bar (mesma plataforma da Philco 20 Bar — mesma carcaça, boiler, hidráulica e grupo; mudam bomba/termostato de vapor).
- **Tipo:** single boiler, bomba vibratória (ULKA ou clone), boiler de alumínio com resistência embutida, termostatos bimetálicos (café ~95°C / vapor ~140°C) e **fusível térmico** de segurança.
- **Alimentação:** **220 V / 60 Hz** (confirmado 30/07/2026). Importa em dois lugares: metade da corrente no SSR em relação a 127 V, e TRIAC de **600 V** obrigatório no dimmer (pico de rede = 311 V).

### Como ela funciona (original)

```
Reservatório → Bomba vibratória → (OPV?) → Boiler → Grupo → Porta-filtro
```

- Temperatura: controlada por **termostato mecânico** (liga/desliga por histerese). **Não há PID.**
- Pressão: **não é controlada** — a bomba gera pressão bruta; quem limita é a resistência do café e a OPV (se existir).

### Modificações atuais (a serem removidas)

- Manômetro analógico de 12 bar, ligado via TEE na linha de pressão (**rosca 1/8" NPT**, mangueira **4mm**).
- Dimmer controlando a **bomba** (não a resistência). Problema relatado: não faz ajuste fino; raramente chega a 9 bar (trava em ~11–12 bar).

> 🔄 **Diagnóstico corrigido (30/07/2026).** Este documento listava "OPV abrindo"
> entre as causas prováveis. **Não pode ser.** O sintoma é não conseguir **descer**
> de 11–12 bar — é um **piso**, não um teto. OPV limita por cima; ela não tem como
> impedir a pressão de baixar.
>
> A causa é **ângulo de fase em potência baixa**: o pulso chega cortado e atrasado,
> o solenoide não completa o curso, e a bomba zumbe sem bombear. É exatamente o
> problema que o [PSM resolve](02-arquitetura-eletrica-pinout.md#controle-da-bomba-dimmer-robotdyn-8a-standard-com-psm).
>
> A pista que parecia apontar para a hidráulica apontava para o dimmer.

> Ambas as modificações serão **substituídas** pela nova eletrônica.

## Decisões de arquitetura (e o porquê)

| Decisão | Escolha | Motivo |
|---|---|---|
| MCU | **ESP32-S3-WROOM-1 N16R8, headless** | Já em mãos. Display sai da Fase 1 → nada bloqueia o início. Sem o board Waveshare perde-se RTC (→ **NTP**) e slot SD (→ **módulo SD**), e ganha-se ~20 GPIOs livres. ⚠️ PSRAM octal inutiliza GPIO33–37. Ver [02](02-arquitetura-eletrica-pinout.md). |
| Sensor de temperatura | **1× PT100 classe A, 3 fios, superfície + 1× MAX31865 (Rref 430 Ω)** | Ver as duas notas abaixo — é a decisão que mais mudou. |
| Leitura analógica | **ADS1115 (ADC 16-bit por I²C), em 5 V** | 16 bits reais e leitura **direta** dos 0,5–4,5 V do sensor sem divisor resistivo. O ADC do ESP32-S3 satura em ~3,1 V. Exige **level shifter** no I²C. |
| Sensor de pressão | **0–13,8 bar (200 PSI)**, saída 0,5–4,5 V, 1/8" NPT | Mais resolução que a faixa de 16 bar. ⚠️ **Não cobre o estol da bomba:** a Ulka vai a ~15–15,5 bar, então acima de 13,8 o sensor grampeia no máximo. Sem risco — com puck real nunca se opera lá — mas a leitura satura em vez de errar silenciosamente. |
| Controle da resistência | SSR-40DA **+ termostato de vapor original em série** | Chaveamento silencioso. O termostato vira *backstop* de hardware contra SSR em curto ou firmware travado. Ver [07](07-seguranca-e-calibracao.md). |
| Controle da bomba | **PSM** (pulos de semiciclo), com `Z-C` e `Dim` direto no ESP32 | 🔄 **Revisto em 29/07/2026** — antes era DimmerLink por UART. Bomba vibratória é solenoide: em potência baixa o ângulo de fase entrega curso incompleto e a bomba **zumbe sem bombear** (é o sintoma relatado na Philco, "não desce de 11–12 bar"). PSM entrega pulso **inteiro** e controla pela contagem. Custo: volta a ISR de zero-cross — mas PSM tolera **centenas** de µs de jitter, contra dezenas do ângulo de fase. Ver [02](02-arquitetura-eletrica-pinout.md#controle-da-bomba-dimmer-robotdyn-8a-standard-com-psm) e [07](07-seguranca-e-calibracao.md). |
| Hidráulica | Padronizada em **1/8" NPT**, mangueira 4mm | Casa com o manômetro/TEE que o usuário já possui. |

### Por que PT100 e não PT1000

A versão anterior deste documento dizia *"maior resolução e menor sensibilidade ao
cabo"*. **A parte da resolução estava errada.** O MAX31865 é ratiométrico: o ADC de
15 bits mede sobre o Rref, e o Rref escala junto com o sensor —
`430/32768 ÷ 0,385 = 0,034 °C/LSB` para PT100 e `4300/32768 ÷ 3,85 = 0,034 °C/LSB`
para PT1000. **Idênticos.** Pela mesma razão, a imunidade a ruído acoplado por
tensão também empata.

A vantagem real do PT1000 é só uma: resistência de cabo e contato pesa 10× menos
(0,385 Ω/°C contra 3,85 Ω/°C). **Com a sonda ligada em 3 fios isso desaparece** — o
modo 3 fios do MAX31865 cancela a resistência de linha, sobrando apenas o
descasamento entre condutores (< 0,05 Ω ≈ **< 0,15 °C** num cabo de 1 m).

Como a sonda em mãos é **PT100 de 3 fios** e o módulo MAX31865 genérico é o de
**Rref 430 Ω** (o padrão de mercado), o conjunto é coerente e a diferença prática
para PT1000 é irrelevante nesta aplicação.

Bônus não planejado: a sonda é de **superfície**, o que dispensa achar rosca livre
no boiler — problema que o [checklist 06](06-engenharia-reversa-checklist.md) tinha
levantado como incógnita. É também o que todos os mods de PID da Philco fazem
(termopar na parede da caldeira).

### Por que **um** sensor de temperatura, e não dois

O plano original tinha um segundo sensor na "saída do grupo" para medir a
temperatura real da água. **Foi removido.** Levantamento do estado da arte:

| Projeto | Sensores de temperatura |
|---|---|
| **Gaggiuino** | **1** — só o boiler (K-type/MAX6675 antigo, NTC nas PCBs novas) |
| PID REX-C100 (mods de Philco, Poemia, Silvia) | **1** — parede da caldeira |
| La Marzocco | 1 + **offset fixo em firmware** |
| **Decent DE1** (~US$ 3,3k) | vários — exceção explicada abaixo |

O DE1 é a exceção que confirma a regra: ele tem sensor no bloco dispersor ~2 mm
acima do puck porque **controla temperatura misturando água quente e fria com duas
bombas em tempo real**. O sensor é a realimentação desse misturador. Sem
misturador, o segundo sensor não tem atuador nenhum para comandar.

**E na Philco o caso é pior:** ela não tem grupo. A caldeira parafusa direto no
chuveiro. Não existe caminho térmico entre "boiler" e "saída do grupo" para
instrumentar — uma sonda de superfície ali leria **o mesmo bloco de alumínio** que
a sonda do boiler. Mediria metal, não água.

Os dados do fórum Clube do Café (medições do usuário *regisassao* com termopar no
bolo) mostram por que um segundo sensor de superfície também não resolveria:

- extração longa: **20 °C** de diferença entre bolo e parede da caldeira;
- extração típica de 28 s: **78 °C no bolo contra 69 °C no PID**.

Repare que **o sinal inverte** — o bolo ora está mais quente, ora mais frio que a
leitura da caldeira, porque a sonda de parede enxerga camadas d'água diferentes
conforme o regime. O offset **não é constante**: varia com vazão e com o instante
do shot.

➡️ **Substituto:** caracterizar o offset **uma vez**, na bancada, com termopar K
entre porta-filtro e grupo, logando temperatura + pressão + tempo em paralelo. Como
os três canais ficam gravados, o offset pode virar depois um modelo função de
vazão/tempo **sem hardware novo**. Procedimento em [07](07-seguranca-e-calibracao.md).

Economia: 1 PT100, 1 MAX31865, 1 GPIO de CS e um furo na máquina.

## Fases do projeto

1. **Documentação + hardware (Fase 1) ✅** — esta documentação.
2. **Bancada headless** — ESP32-S3 + sensores na mesa, sem a máquina: I²C scan, leitura
   de temperatura e pressão, teste de SSR e dimmer com lâmpada. **Não depende de abrir a máquina.**
3. **Engenharia reversa física** — abrir a máquina, confirmar bomba/OPV/rosca e o
   estado do parafuso do chuveiro ([06](06-engenharia-reversa-checklist.md)).
4. **Instrumentação** — instalar sensores na máquina + caracterizar o offset de
   temperatura com termopar K ([07](07-seguranca-e-calibracao.md)).
5. **Controle** — PID de temperatura + malha de pressão.
6. **Firmware** — arquitetura modular, web UI, API ([05](05-firmware-arquitetura.md)).
7. **Interface local** — comprar o board com touchscreen e portar a UI para LVGL.
8. **Avançado** — perfis, gráficos em tempo real, histórico, OTA, (opcional) Home Assistant.

> As fases 2 e 3 são **paralelas**: a bancada eletrônica anda enquanto a Leva 2
> (hidráulica) não chegou.

## Estratégia de montagem

- Protótipo com **módulos "voando"** conectados, presos por uma **peça impressa 3D** que organiza tudo.
- Firmware desenhado **como se já fosse uma PCB** desde o início (separação lógica entre "Control Board" — sensores/atuadores/segurança/tempo-real — e "UI Board" — Wi-Fi/API, depois touchscreen), mesmo rodando num único ESP32.
- Quando o protótipo estiver validado, migrar para **PCB** (KiCad) reaproveitando o mapa de pinos de [02](02-arquitetura-eletrica-pinout.md).
