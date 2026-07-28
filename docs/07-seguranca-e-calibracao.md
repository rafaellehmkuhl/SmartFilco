# 07 — Segurança e calibração

## ⚠️ Segurança elétrica (rede AC 127/220V)

Este projeto trabalha com **tensão de rede** e **água quente sob pressão**. Erros
podem causar choque, incêndio ou queimadura.

- **Sempre desligar da tomada** antes de mexer. Não confie só na chave.
- **Manter o fusível térmico original** do boiler (~165 °C) — é a última proteção
  contra superaquecimento. **Nunca** remover ou "pontear". Se ele abrir, já houve
  falha grave a montante.
- **Isolação:** todo o lado de baixa tensão (ESP32, ADS1115, MAX31865) é isolado do
  AC pelos **optoacopladores** do SSR e do dimmer e pela **fonte isolada Hi-Link**.
  Não criar caminhos que quebrem essa isolação.
- **Aterramento:** chassi metálico e partes condutoras **aterrados**. Água + eletricidade exige terra.
- **Dimensionamento:** SSR-40DA folgado para a resistência (~1000–1200 W ≈ 8–9,5 A
  em 127 V), **com dissipador**; fios de potência AWG adequado; Faston isolados.
- **Separação física:** manter a eletrônica de sinal longe do boiler e dos fios de potência; roteirizar AC e sinal separados.

### 🔒 Backstop de hardware: termostato de vapor em série com o SSR

**Esta é uma decisão de projeto, não uma sugestão.** O SSR é um dispositivo de
estado sólido e seu modo de falha mais comum é **em curto** — resistência ligada
permanentemente, sem nenhum comando. Somado a isso, um travamento de firmware
deixa a saída no último estado. Em ambos os casos, sem backstop, o boiler sobe até
o fusível térmico abrir — e aí é **caldeira nova**, que é justamente o custo que
mais mata essa máquina.

Topologia adotada:

```
Fase ──► SSR-40DA ──► TERMOSTATO DE VAPOR original (~135–145 °C, NF) ──► Resistência ──► Neutro
                             (backstop)
         (fusível térmico ~165 °C permanece onde está, intocado)
```

- O **termostato de extração** (~95 °C) sai do circuito — quem controla a extração
  agora é o PID.
- O **termostato de vapor** (~135–145 °C) passa a ficar **permanentemente em série**
  com a resistência. Ele nunca abre durante extração (setpoint 90–100 °C), mas
  limita a caldeira **antes** do fusível térmico se o controle falhar.
- Bônus: no modo vapor ele vira o limitador natural — o firmware sobe o setpoint e
  o termostato define o teto, exatamente como na máquina original.

Essa é a mesma topologia que a comunidade de mods da Philco convergiu (instalação
que mantém o termostato de vaporização em vez de desativar tudo).

⚠️ **Confirmar ao abrir a máquina** ([06](06-engenharia-reversa-checklist.md)):
na fiação original os dois termostatos são **selecionados pela chave de vapor**, não
estão os dois em série. Colocar o de vapor permanentemente em série exige rewiring —
mapear antes de cortar.

> **Corrente de fuga:** SSRs deixam passar alguns mA mesmo desligados. A resistência
> **nunca fica 100% desenergizada** com a máquina na tomada. O SSR não é dispositivo
> de seccionamento — para manutenção, **tire da tomada**.

### Proteções por software (SafetyModule), independentes da UI

- Corte se `T_boiler` > limite (ex.: 160 °C) ou sensor **aberto/curto** (o MAX31865
  tem detecção de falha por hardware — usar o registrador de fault, não só heurística).
- **Sensor travado:** se a leitura não variar por N segundos com a resistência
  ligada, tratar como falha (sonda descolada do boiler é o caso real).
- **Timeout da bomba** (não bombear a seco / tempo máximo de shot).
- Limite de **pressão** máxima.
- **Watchdog** reiniciando o loop de controle se travar.
- Estado seguro no boot e em falha: **SSR e bomba desligados**.

## Calibração dos sensores

### Temperatura (PT100 3 fios + MAX31865)

Configuração do módulo, **antes de ligar**:

- **Rref = 430 Ω** (PT100). Confirmar pela marcação do resistor SMD:
  `4300` ou `431` → 430 Ω ✅ · `4301` ou `432` → 4300 Ω (é PT1000, placa errada).
- **Modo 3 fios** — nas placas estilo Adafruit são pads de solda que precisam ser
  reconfigurados; de fábrica vêm em 2/4 fios. Rodar em 2 fios com sonda de 3 fios
  funciona, mas joga fora a compensação de linha.
- Coeficiente da curva: **α = 0,00385** (padrão europeu/IEC 60751), que é o que as
  bibliotecas assumem por default.

Verificação:

- Referências conhecidas: **gelo fundente ~0 °C** e **água em ebulição**, corrigida
  pela altitude — em São Paulo (~760 m) a ebulição fica em ~97,4 °C, não 100 °C.
- Aplicar **offset** se necessário. Esperar erro pequeno: PT100 classe A tem
  tolerância de ±(0,15 + 0,002·|t|) °C, ≈ ±0,34 °C a 95 °C.

### Caracterização do offset boiler-bolo

O sensor mede a **parede do boiler**, não a água que chega ao café. Esse offset
existe, é grande e **não é constante** — varia com vazão e com o instante do shot.
Medições de referência no fórum Clube do Café (termopar K no bolo, máquinas
equivalentes): até **20 °C** de diferença em extração longa, e num shot típico de
28 s **78 °C no bolo contra 69 °C na parede** — repare que **o sinal inverte**
conforme o regime.

Por isso não se resolve com um segundo sensor de superfície (ver
[01](01-projeto-visao-geral.md#por-que-um-sensor-de-temperatura-e-não-dois)); resolve-se
medindo uma vez e modelando:

1. Termopar K fino espremido **entre porta-filtro e grupo** (técnica documentada no
   fórum; atrapalha um pouco a vedação — tenha um pano à mão).
2. Extrair com o firmware **logando em paralelo**: `t, T_boiler, pressão, potência da bomba`.
3. Anotar a leitura do termopar ao longo do shot.
4. Repetir variando **vazão** e **tempo de pré-aquecimento** — são as duas variáveis
   que mais movem o offset.
5. Ajustar o modelo. Começar por um **offset escalar** (simples, é o que Gaggiuino e
   La Marzocco fazem) e só evoluir para `offset = f(vazão, tempo)` se os dados
   mostrarem que vale.

> Como os três canais ficam gravados no microSD, o modelo pode ser refinado depois
> **sem hardware novo** e sem repetir a bancada.

### Pressão (sensor 0,5–4,5V via ADS1115)

- Faixa real do sensor comprado: **200 PSI = 13,8 bar**.
- Curva teórica: `P(bar) = (Vout − 0,5) / (4,5 − 0,5) × 13,8`
- **PGA do ADS1115 em ±6,144 V** — em qualquer ganho maior o sinal de 4,5 V satura.
  Resolução resultante: 187,5 µV/LSB ≈ **0,00065 bar/contagem**. O ruído do sensor e
  a pulsação da bomba vibratória limitam muito antes do ADC — prever **filtro** (média
  móvel ou IIR) na leitura, senão a malha de pressão persegue a ondulação de 60 Hz.
- Calibrar contra o **manômetro analógico** (mesmo ponto no TEE): anotar pares
  (V lido, bar no manômetro) e ajustar ganho/offset.
- `0,5V → 0 bar`, `4,5V → 13,8 bar` são nominais; medir os reais.

## Calibração e controle da bomba (o ponto mais delicado)

### Por que é difícil

Bombas vibratórias (ULKA e clones) têm resposta **muito não-linear**: 50% de
potência **não** dá 50% da pressão. Por isso o dimmer manual atual não faz ajuste
fino e "trava" em ~11–12 bar sem chegar aos 9 bar desejados.

### Método de controle — decidido pelo hardware

O módulo comprado tem o **DimmerLink integrado**, que faz **controle por ângulo de
fase** e só isso. Isso fecha a questão:

1. **Ângulo de fase (TRIAC):** ✅ é o que temos. Reduz torque em potências baixas e
   pode fazer a bomba parar de bombear antes do ponto desejado — limitação a
   conhecer, não a contornar.
2. ~~**Burst firing / ciclos inteiros**~~ ❌ **fora de alcance.** O DimmerLink não
   expõe modo de ciclos inteiros; o disparo é interno e por fase. Era a alternativa
   preferida em teoria para bombas vibratórias, mas exigiria trocar o módulo por um
   dimmer clássico e reimplementar a ISR no ESP32 — troca ruim, dado que o
   DimmerLink elimina todo o jitter de timing do firmware.
3. **Malha fechada (o que faz o trabalho):** fechar o laço `pressão → PID → bomba`
   corrige a não-linearidade automaticamente e é o que permite *pressure profiling*
   real (2 bar de pré-infusão → 9 bar → declínio). Com só o item 1 disponível, a
   malha fechada deixa de ser "recomendada" e passa a ser **essencial**.

### Curva de dimerização — testar RMS primeiro

O DimmerLink oferece **Linear**, **RMS** e **Logarítmica**. "Linear" é linear no
*ângulo de fase*, que não corresponde a nada de útil fisicamente. A curva **RMS**
mapeia o comando para tensão RMS, muito mais próxima da grandeza que move um
solenoide — ou seja, já lineariza boa parte do processo **antes** do PID.

Incluir a escolha de curva na caracterização abaixo: levantar a tabela
`pressão × comando` nas três curvas e ficar com a mais próxima de uma reta. Quanto
mais linear o processo, mais simples a sintonia e mais estável em toda a faixa.

### Procedimento de caracterização (na bancada, com a máquina instrumentada)

1. Com o sensor de pressão já lendo, varrer o comando da bomba de 0 a 100 **de 1 em 1**
   (a resolução do DimmerLink é inteira — não há passo mais fino).
2. Registrar **pressão × comando** (com porta-filtro cego / restrição conhecida).
3. **Repetir nas três curvas** do DimmerLink (Linear / RMS / Log) e escolher a mais linear.
4. Anotar o **comando mínimo em que a bomba realmente bombeia** — abaixo disso ela
   zumbe sem vazão, e o PID precisa saturar ali em vez de continuar reduzindo.
5. Montar a **LUT de calibração** (feedforward) e usá-la como ponto de partida do PID.
6. Ajustar ganhos do PID de pressão para resposta estável, sem oscilação.

### 🚦 Critério de decisão: fica no DimmerLink ou migra para PSM?

Esta caracterização **é o experimento que decide** se o dimmer comprado atende. Os
dados a extrair e o critério:

| Medir | Reprovado se |
|---|---|
| Nº de degraus inteiros entre **2 bar e 9 bar** | **< 15 degraus** — pouca autoridade para perfilar |
| Estabilidade da pressão em **2–3 bar** (pré-infusão) | oscilação/pulsação grosseira, ou a bomba alterna entre zumbir e jorrar |
| Comando mínimo com vazão real | muito alto, deixando a pré-infusão inalcançável |
| Repetibilidade subindo × descendo o comando | histerese grande |

- ✅ **Passou:** segue no DimmerLink. Zero solda, zero ISR crítica no firmware.
- ❌ **Reprovou:** migrar para **PSM**, com o bypass documentado em
  [02](02-arquitetura-eletrica-pinout.md#plano-b--bypass-do-dimmerlink-para-psm).

Ao avaliar a migração, os contras **reais** do PSM são **ripple de pressão** em duty
baixo e a volta da ISR de timing crítico ao firmware. Ignore a afirmação de que PSM
travaria ou queimaria bomba vibratória — é falsa, e desmontada em
[02](02-arquitetura-eletrica-pinout.md#-mito-psm-trava-e-queima-bomba-vibratória).

O ponto fraco previsível é a **pré-infusão**, não a extração. Ângulo de fase em
potência baixa é justamente onde bomba vibratória se comporta pior — o solenoide não
completa o curso. Extração a 9 bar deve ir bem em qualquer cenário.

> Não antecipe a migração. O bypass exige dessoldar componente em placa de rede AC e
> devolve a ISR de timing crítico ao firmware — só vale com dado mostrando que
> precisa.

### Interação com a OPV

- Se a OPV abrir cedo, ela limita a pressão máxima e **atrapalha** o profiling.
- Estratégia: regular a OPV como **teto de segurança** (ex.: ~11–12 bar) e deixar o
  perfil de pressão por conta do controle eletrônico da bomba.
- Confirmar na engenharia reversa se **já existe** OPV e sua pressão ([06](06-engenharia-reversa-checklist.md)).

## Checklist de "primeiro ligamento" seguro

**Bancada (sem a máquina):**

- [ ] Bulk de 470–1000 µF instalado no trilho de 5 V.
- [ ] Alimentar só a eletrônica (Hi-Link) **sem** conectar resistência/bomba ao AC.
- [ ] ESP32 sobe sem brownout, conecta ao Wi-Fi e sincroniza NTP.
- [ ] **I²C scan** OK através do level shifter (ADS1115 em 0x48). Se não aparecer, o
      suspeito nº1 é pull-up/alimentação no lado HV do shifter.
- [ ] MAX31865: Rref 430 Ω confirmado, modo 3 fios configurado, leitura de ambiente coerente.
- [ ] PGA do ADS1115 em ±6,144 V **antes** de ligar o sensor de pressão.
- [ ] microSD monta e grava (SPI compartilhado com o MAX31865 → conferir que ambos
      leem alternadamente sem se corromper).
- [ ] Testar SSR e dimmer com **carga de teste** (lâmpada) antes da resistência/bomba reais.
- [ ] Validar proteções: simular sensor aberto (desconectar a sonda) → sistema deve cortar.

**Na máquina:**

- [ ] Termostato de vapor confirmado **em série** com o SSR (backstop) e fusível térmico intacto.
- [ ] Chassi aterrado.
- [ ] Sonda PT100 com pasta térmica e bem fixada — sonda solta é falha silenciosa.
- [ ] Primeiro aquecimento **supervisionado**, com termômetro independente conferindo, e
      setpoint conservador (ex.: 85 °C) antes de confiar na malha.
- [ ] Só então cargas reais em uso normal, com extintor por perto.
