# 04 — BOM / Lista de compras

> **Status (jul/2026):** Leva 1 **fechada**. A eletrônica está comprada ou já em
> mãos; falta só miudeza de loja local. A Leva 2 (hidráulica) continua dependendo
> da engenharia reversa ([06](06-engenharia-reversa-checklist.md)).

Decisão do usuário: **sem redundância** (1 de cada). Risco assumido: se o sensor de
pressão vier com defeito, são +2 semanas de espera.

---

## ✅ Leva 1 — Eletrônica

### Comprado no AliExpress (total R$ 201,94)

| Item | Preço | Notas |
|---|---|---|
| **Dimmer AC RobotDyn 8A**, variante **UART/I²C (V2)** — traz o **DimmerLink** integrado | R$ 32,17 | 🔄 **Comprado, mas não será usado como veio.** A decisão de 29/07/2026 é atuar a bomba por **PSM**, o que dispensa o DimmerLink. Vira reserva — o clássico com pinos ZC/PWM é a peça certa, ver [Futuro](#-dimmer-comprar-o-clássico-para-psm) e [02](02-arquitetura-eletrica-pinout.md#bypass-do-dimmerlink-para-psm-decidido) |
| **ADS1115** 16-bit I²C | R$ 11,99 | alimentar em **5 V**, PGA **±6,144 V** |
| **Sensor de pressão 200 PSI**, 1/8" NPT, 5 V | R$ 45,39 | = **13,8 bar**. Rosca já é NPT ✅ |
| **Cabo de silicone** (branco/vermelho/preto) | R$ 28,72 | |
| **Emendas termo-retráteis com solda** (50 pçs) | R$ 12,90 | |
| **Dissipador para SSR** | R$ 11,25 | não estava na BOM — **acerto**, SSR-40DA sem dissipador não segura a resistência |
| **PT100 classe A, superfície, 3 fios, alta temp., 1 m** | R$ 21,29 | ⚠️ é PT100, não PT1000 — decisão revisada, ver [01](01-projeto-visao-geral.md#por-que-pt100-e-não-pt1000) |
| **SSR-40DA** | R$ 17,99 | 900 avaliações / 4,9 · usuário testou 1400 W com dissipador. Resistência da Philco ~1000–1200 W → folgadíssimo |
| **MAX31865** | R$ 20,24 | ⚠️ **confirmar Rref = 430 Ω** (marcação `4300`/`431`) e **configurar 3 fios** |

### Já em mãos (não comprar)

| Item | Observação |
|---|---|
| **ESP32-S3-WROOM-1 N16R8**, dual USB-C | substitui o Waveshare na Fase 1 — projeto começa **headless**. ⚠️ PSRAM octal: **GPIO33–37 inutilizáveis** apesar de estarem no header |
| **Fonte HLK-5M05** (AC→5V, 5W) | |
| **Level shifter bidirecional** | obrigatório: ADS1115 em 5 V × I²C 3,3 V do ESP32 |
| **Módulo microSD SPI** + cartão | o devkit não tem slot; compartilha o bus SPI com o MAX31865 |
| **Manômetro analógico** (12 bar) | já instalado na máquina |
| **Terminais Faston / conectores da máquina** | serão reaproveitados |

### ❌ Ainda falta (loja local, trivial)

| Item | Por quê |
|---|---|
| **Capacitor eletrolítico 470–1000 µF / 10 V** | bulk no trilho de 5 V. O HLK-5M05 dá 1 A contínuo, mas bursts de TX do Wi-Fi passam disso. Sem ele → brownout aleatório no boot |
| **Termopar K + medidor** (ou multímetro com entrada K) | só para caracterizar o offset uma vez ([07](07-seguranca-e-calibracao.md)). ~R$ 40 num TM-902C |
| Bornes **WAGO**, tubo termo-retrátil | junções e distribuição |
| Parafusos **M3**, inserts de latão, espaçadores | fixação dos módulos na peça 3D |
| **Pasta térmica** | contato da PT100 com o boiler |
| Filamento **PETG** | suporte impresso (PETG pela temperatura interna) |

### 🗑️ Removido da lista

| Item | Motivo |
|---|---|
| ~~Waveshare ESP32-S3-Touch-LCD-3.5~~ | adiado para a fase de interface — ESP32-S3 em mãos cobre a Fase 1 |
| ~~2ª PT1000 + 2º MAX31865~~ | segundo sensor de temperatura **cortado do projeto**. Nenhum projeto DIY de espresso usa dois; na Philco não há caminho térmico para instrumentar. Ver [01](01-projeto-visao-geral.md#por-que-um-sensor-de-temperatura-e-não-dois) |
| ~~DC-DC MP1584~~ | era opcional; nada exige 3,3 V próprio |
| ~~Cartão microSD~~ | já em mãos |
| ~~Terminais Faston~~ | reaproveitando os da máquina |

### Futuro (não precisa agora)

| Qtd | Item | Buscar por | Função |
|---|---|---|---|
| ~~1~~ | ~~Flow meter Hall (YF-S401)~~ | **REMOVIDO** | Ver abaixo — não mede espresso, e com PSM não é preciso |
| 2 | **ACS712** (20A) | "ACS712 20A module" | diagnóstico de corrente (resistência/bomba) |
| 1 | Sensor de nível (boia magnética) | "magnetic float switch" | evitar bomba a seco |
| 1 | Display para a fase de interface | ver nota abaixo | Waveshare 3.5" SPI/ST7796 **ou** placa integrada com display RGB |

### ❌ Flow meter YF-S401 — fora do projeto

Dois motivos independentes, e o primeiro já bastaria:

**1. Não mede espresso.** O YF-S401 é especificado para **0,3–6 L/min**, com precisão
de 3% apenas entre 0,15 e 3 L/min ([datasheet](https://www.quartz1.com/price/PIC/250Q1442301.pdf)).
Vazão de espresso:

| Shot | Vazão |
|---|---|
| 36 g em 30 s | **0,072 L/min** |
| 50 g em 25 s (rápido) | **0,12 L/min** |

Ou seja: **2,5 a 4× abaixo do mínimo do sensor**, e abaixo até do piso de precisão. A
turbina não gira de forma confiável nessa faixa. Ele só leria durante *flush* sem
bolo — exatamente quando a medida não serve para nada. E não ajuda montar antes da
bomba: vazão volumétrica é a mesma dos dois lados.

**2. Com PSM não é preciso.** Cada semiciclo que passa é um curso discreto da bomba,
então vazão sai de **contagem de pulsos + pressão**, calibrada contra balança. É assim
que o Gaggiuino faz *flow profiling* — e ele **não** usa flow meter. Máquinas
comerciais usam medidor volumétrico de baixa vazão classe Gicar; o Decent DE1 usa
sensor feito sob medida. Turbina de irrigação não é a peça.

> Isto reverte a nota anterior, que marcava o flow meter como "mais necessário do que
> parecia". Aquele raciocínio valia **porque** o DimmerLink só faz ângulo de fase.
> Com a [decisão de ir para PSM](02-arquitetura-eletrica-pinout.md#bypass-do-dimmerlink-para-psm-decidido)
> o atalho de contagem de pulsos volta a existir, e o sensor perde a razão de ser.
>
> Se algum dia houver medição de vazão de verdade, o critério é **faixa**: precisa
> ler ~0,05–0,3 L/min, não 0,3–6.

### 🛒 Dimmer: comprar o **clássico** para PSM

O V2 com DimmerLink integrado já está comprado, mas é a variante errada para PSM —
chegar nos sinais `Z-C`/`Dim` nele exige dessoldagem em placa de rede AC. O **dimmer
RobotDyn clássico expõe `Z-C` e `PWM` em header**, prontos, por ~R$ 32.

| Qtd | Item | Buscar por | Função |
|---|---|---|---|
| 1 | **Dimmer AC RobotDyn 8A clássico** (pinos ZC/PWM) | "RobotDyn AC dimmer 8A ZC PWM" | bomba por PSM. ⚠️ **Não** a variante UART/I²C V2 |

Guardar o V2 como reserva. Detalhes em
[02](02-arquitetura-eletrica-pinout.md#bypass-do-dimmerlink-para-psm-decidido).

---

## 🔧 Leva 2 — Hidráulica (comprar após abrir a máquina)

Padronizar tudo em **1/8" NPT** + mangueira **4 mm**.

| Qtd | Item | Notas |
|---|---|---|
| 1 | **TEE 1/8" NPT** | ponto de pressão: manômetro + sensor |
| 1 | **OPV ajustável** para espresso | confirmar entrada/saída 1/8" NPT; regular como teto de segurança |
| — | Adaptadores **1/8" NPT → espigão/push-fit 4 mm** | conexão às mangueiras |
| — | Mangueira de silicone/PTFE alta temperatura, 4 mm | comprimento conforme a máquina |
| — | **Fita veda-rosca PTFE** | vedação das roscas NPT |
| 1 | (contingência) **parafuso do chuveiro** em inox/sextavado | ver [06](06-engenharia-reversa-checklist.md) — é a peça que mais mata essa máquina |

> O **adaptador BSP→NPT** saiu da lista: o sensor comprado já é 1/8" NPT.

> **Antes de fechar a Leva 2, confirme na máquina:** se já há OPV (e sua pressão),
> macho/fêmea de cada rosca, espaço físico para TEE+OPV+sensor+manômetro, e o
> comprimento de mangueira. Checklist completo em [06](06-engenharia-reversa-checklist.md).
