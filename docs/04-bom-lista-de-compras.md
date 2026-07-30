# 04 — BOM / Lista de compras

> **Status (30/07/2026): a BOM fechou.** Leva 1 encomendada em 26/07 (chega ~05/08),
> Leva 2 já estava em mãos de projetos anteriores, e a OPV saiu do projeto.
>
> **Sobra uma única compra no AliExpress: o dimmer RobotDyn `8A-Standard`**
> ([modelo exato](02-arquitetura-eletrica-pinout.md#modelo-exato-a-comprar)) — ~10 dias
> a partir da encomenda. Fora isso, só miudeza de loja local.
>
> ⚠️ **O caminho crítico deixou de ser peça: virou abrir a máquina.** A
> [`06`](06-engenharia-reversa-checklist.md) não depende de compra nenhuma e destrava
> as últimas perguntas em aberto (modelo da bomba → `divider` do PSM, existência da
> OPV, roscas). Pode ser feita hoje.

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
| **Fonte HLK-5M05** (AC→5V, 5W) | ✅ **em mãos** (projeto anterior). Basta para a Fase 1 headless, mas **não** para a fase de interface: ver nota de fonte abaixo |
| **Level shifter bidirecional** | ✅ **em mãos** (projeto anterior). Obrigatório: ADS1115 em 5 V × I²C 3,3 V do ESP32 |
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
| 1 | **GUITION JC4880P443C** — ESP32-P4 + C6, 4,3" 480×800 MIPI-DSI | [item 1005009618259341](https://pt.aliexpress.com/item/1005009618259341.html) | **placa única**: controle + interface. Ver nota abaixo |
| 1 | **Fonte AC→5V de 10 W** | "HLK-10M05" | substitui a de 5 W quando o display entrar |

### 🖥️ Display: **GUITION JC4880P443C (ESP32-P4, 4,3")** — uma MCU só

> 🔄 **Revisto em 30/07/2026.** Em 29/07 a decisão era comprar uma placa RGB e usá-la
> como **UI Board separada**, ao lado da DevKitC-1 — porque nenhuma placa integrada
> tinha pino sobrando. Pesquisa posterior encontrou uma que tem.

**O que mata uma placa integrada é a interface do display, não o display embutido:**

| Interface | Display come | Sobra na placa |
|---|---|---|
| **RGB** paralelo | ~20 GPIOs + 3 do touch | **1** ❌ |
| **SPI** (ST7796) | 5–7 | 12 ✅ |
| **MIPI-DSI** (P4) | **1** (só o reset) | **12 + I²C dedicado** ✅ |

| Opção | Tela | Preço | Controlador? |
|---|---|---|---|
| **GUITION JC4880P443C**, ESP32-P4+C6, 4,3" 480×800 DSI | maior | **R$ 167 + 34** | ✅ **recomendada** |
| Waveshare ESP32-S3 3,5" SPI/ST7796, 320×480 | menor | R$ 179 + ~36 | ✅ viável (12 livres, 2 são strapping) |
| Waveshare/SpotPear 4,3" **RGB** 800×480 | grande | R$ 167 | ❌ 1 GPIO livre |
| GUITION JC8048W550 5" **RGB** | maior | R$ 168 | ❌ idem |
| Display "burro" ST7796 avulso | — | R$ 135 | ❌ **mais caro que placa integrada com o mesmo painel** |

**Por que a P4 ganha:** cabe em 7 dos 12 pinos livres (sobram 5), tem tela maior que a
3,5", custa menos que a Waveshare, e o **Wi-Fi mora num ESP32-C6 separado** — o que
tira do chip de controle a parte de tempo real duro do rádio. Mapa completo do header
em [02](02-arquitetura-eletrica-pinout.md#o-header-de-26-pinos-da-jc4880p443c).

⚠️ **Antes de comprar:**
- Conferir quais dos GPIOs 28–35 / 49–52 são **strapping** no ESP32-P4 — `Z-C` e `Dim`
  não podem cair num deles.
- Confirmar se há **slot TF** (a Waveshare P4 tem; esta não foi confirmada). Sem ele o
  microSD compartilha o SPI do MAX31865 e custa +1 CS — continua cabendo.
- A pinagem oficial **não é publicada**: mora num ZIP em file host chinês. O mapa que
  temos veio da imagem de "PIN DEFINITIONS" do anúncio.

### ⚡ Fonte: 5 W não aguenta as duas placas

A Waveshare especifica **450 mA** só para a placa do display. Somando o ESP32 de
controle em pico de TX de Wi-Fi, passa de **750 mA**. A **HLK-5M05 dá 1 A** — margem
fina demais para uma fonte de rede AC dentro de máquina quente.

➡️ Subir para **10 W** (HLK-10M05) quando o display entrar. Na Fase 1 headless a de
5 W está folgada e não precisa trocar agora.

> O capacitor de bulk continua obrigatório nas duas, e por motivo diferente do
> dimensionamento médio: o problema é o **burst** de TX, não a corrente contínua.

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
| **2** | **Dimmer AC RobotDyn `8A-Standard`** | [item 32802025086](https://pt.aliexpress.com/item/32802025086.html) | bomba por PSM. Header `VCC GND Z-C `**`PSM`** (a serigrafia diz PSM, não PWM). TRIAC **BTA16-600B de aba isolada**. Comprar 2: o frete custa mais que a peça. ⚠️ **Não** a variante UART/I²C V2 |

Guardar o V2 como reserva. Detalhes em
[02](02-arquitetura-eletrica-pinout.md#bypass-do-dimmerlink-para-psm-decidido).

---

## ✅ Leva 2 — Hidráulica — **fechada, nada a comprar**

Padronizada em **1/8" NPT** + mangueira **4 mm**.

| Qtd | Item | Situação |
|---|---|---|
| 1 | **TEE 1/8" NPT** | ✅ **em mãos** — já instalado no manômetro analógico |
| — | Adaptador **1/8" NPT → 4 mm** | ✅ **em mãos** — em uso no manômetro |
| — | Mangueira 4 mm alta temperatura | ✅ **em mãos** |
| — | **Fita veda-rosca PTFE** | ✅ **em mãos** |
| ~~1~~ | ~~**OPV ajustável**~~ | ❌ **decidido não comprar** — ver abaixo |
| 1 | (contingência) **parafuso do chuveiro** em inox/sextavado | só se quebrar — ver [06](06-engenharia-reversa-checklist.md) |

> O **adaptador BSP→NPT** saiu da lista: o sensor comprado já é 1/8" NPT.

### ❌ OPV ajustável — fora do projeto (30/07/2026)

Resumo: **a pressão de estol da própria Ulka (~15 bar) já é o teto de segurança, e a
máquina é projetada para ele.** Não existe modo de falha em que o software faça a
bomba passar disso. E uma OPV regulada baixo brigaria com o controle em malha
fechada, que é quem define a pressão de extração aqui.

Argumento completo em
[03](03-esquema-hidraulico.md#-decisão-30072026-não-comprar-opv-ajustável). Se ao
abrir a máquina existir uma OPV, a ação é no máximo **ajustar a que já está lá** —
nunca adicionar outra.

> A ressalva antiga ("antes de fechar a Leva 2, confirme na máquina roscas e espaço")
> perdeu o objeto: as peças já estavam em mãos de projetos anteriores e já estão
> instaladas e funcionando no manômetro analógico. O que sobra para a abertura é a
> pergunta da OPV, que não bloqueia compra nenhuma.
