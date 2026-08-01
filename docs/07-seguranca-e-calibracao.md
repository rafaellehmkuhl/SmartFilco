# 07 — Segurança e calibração

## ⚠️ Segurança elétrica (rede AC **220 V / 60 Hz**)

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
- **Dimensionamento:** com **220 V confirmado**, a resistência (~1000–1200 W) puxa
  **4,5–5,5 A** — metade do que este documento supunha ao considerar 127 V (8–9,5 A).
  O SSR-40DA fica absurdamente folgado. **Manter o dissipador mesmo assim**: SSR sem
  dissipador é falha clássica, e ele já está comprado. Fios de potência em AWG
  adequado; Faston isolados.
- ⚠️ **220 V é mais perigoso que 127 V.** Mesma corrente de fibrilação, quase o dobro
  da tensão para empurrá-la. Nada muda no projeto (fonte isolada, aterramento, caixa),
  mas o descuido custa mais caro.
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

### Método de controle — **PSM** (revisto em 29–30/07/2026)

> 🔄 Esta seção dizia que o hardware comprado "fecha a questão" em favor do ângulo de
> fase, e descartava ciclos inteiros como *"fora de alcance"* por exigir trocar o
> módulo. **A conclusão se inverteu**: o dimmer clássico custa ~R$ 23, e a troca
> ficou barata perto do que o ângulo de fase custa em desempenho.

1. ~~**Ângulo de fase**~~ ❌ **descartado.** Reduz torque em potência baixa a ponto de
   a bomba **zumbir sem bombear** — é a causa real do platô de 11–12 bar relatado
   nesta máquina ([01](01-projeto-visao-geral.md)). O defeito é físico, não de ajuste.
2. **PSM (pulos de semiciclo):** ✅ **adotado.** Todo pulso que passa é **inteiro e em
   força total**; controla-se pela **contagem**. Comportamento em baixa potência é
   muito melhor, que é exatamente onde a pré-infusão vive.
   Ver [02](02-arquitetura-eletrica-pinout.md#bypass-do-dimmerlink-para-psm-decidido).
3. **Malha fechada (o que faz o trabalho):** fechar o laço `pressão → PID → bomba`
   corrige a não-linearidade e é o que permite *pressure profiling* real. Continua
   essencial — o PSM melhora o atuador, não substitui o controle.

> ⚠️ **Medir a frequência da rede, não fixar no código.** A rede é **220 V / 60 Hz**
> (semiciclo = 8,33 ms), mas o firmware não deve assumir: a `PSM.Library` tem `cps()`
> para contar cruzamentos por segundo, e é proteção barata contra uma suposição
> errada — inclusive se a máquina rodar em gerador ou em outra região.

### ~~Curva de dimerização~~ — não se aplica ao PSM

> 🔄 **Superado.** Esta seção discutia as curvas Linear/RMS/Logarítmica do DimmerLink
> e mandava escolher a mais linear. **Com PSM não há curva a escolher:** o comando é
> a fração de semiciclos liberados, e a relação comando → energia é **linear por
> construção**. O que resta de não-linearidade é da bomba e do puck, e é isso que a
> LUT + PID resolvem.
>
> Sobra uma escolha equivalente, essa sim relevante: o **`range`** do acumulador, que
> define resolução × tempo de resposta. Com 120 semiciclos/s e `range=127` o padrão
> fecha em ~1,06 s.

### Procedimento de caracterização (na bancada, com a máquina instrumentada)

1. Com o sensor de pressão já lendo, varrer o comando da bomba de 0 a `range` **de 1
   em 1** (a resolução é o número de semiciclos da janela).
2. Registrar **pressão × comando** (com porta-filtro cego / restrição conhecida).
3. Repetir com **`divider=1` e `divider=2`** — se a bomba tiver o diodo interno
   (EP5/EX5 têm), `divider=2` deve dar resposta visivelmente mais linear.
4. Anotar o **comando mínimo em que a bomba realmente bombeia** — abaixo disso ela
   zumbe sem vazão, e o PID precisa saturar ali em vez de continuar reduzindo.
5. **Medir a constante de tempo de descida:** estabilizar em 9 bar com puck real,
   cortar a bomba e cronometrar a queda. É esse número que define se um perfil
   declinante é factível e com que taxa máxima ([03](03-esquema-hidraulico.md)).
6. Montar a **LUT de calibração** (feedforward) e usá-la como ponto de partida do PID.
7. Ajustar ganhos do PID de pressão para resposta estável, sem oscilação.

> ### 🔄 O diodo da bomba é **interno** (corrigido em 30/07/2026)
>
> Este documento dizia "Ulkas normalmente têm um diodo em série" e mandava
> **procurá-lo na fiação ao abrir a máquina**. Está errado no método: o diodo
> (**1N4007P**) é **integrado ao corpo da bomba** nas Ulka **EP5 e EX5**. Não é
> escolha do fabricante da cafeteira, e não aparece no chicote.
>
> ➡️ **A verificação vira identificar o modelo da bomba**, não caçar componente. A
> [`06`](06-engenharia-reversa-checklist.md) já pede foto da etiqueta — é isso que
> responde.
>
> **Consequência prática:** a bomba trabalha em meia onda, então **metade dos
> semiciclos não produz curso nenhum**. Para o PSM isso não é detalhe: liberar um
> semiciclo da polaridade bloqueada gasta uma decisão de "ligar" em nada, e o
> mapeamento comando → vazão fica não-linear.
>
> ➡️ **`divider = 2`** na PSM.Library resolve — como os eventos de ZC alternam
> polaridade, decidir a cada dois trava na polaridade que conduz. Detalhes em
> [02](02-arquitetura-eletrica-pinout.md#-divider--2-é-a-resposta-ao-diodo-da-bomba).
>
> Sendo EP5/EX5, `divider=2` é o **padrão esperado**, não um caso condicional.
>
> Relatos de perda de eficácia abaixo de ~20% de duty continuam valendo como
> expectativa para a caracterização.

### 🚦 Critério de decisão — ⚠️ resolvido antes do experimento

> **Decisão tomada em 29/07/2026: vamos de PSM.** Este critério não é mais um portão
> a atravessar; ficou registrado porque **a caracterização continua valendo**, agora
> como linha de base para comparar contra o PSM, não como veredito sobre o DimmerLink.
>
> O que pesou para antecipar: o ponto fraco previsível do ângulo de fase é exatamente
> a pré-infusão, e a física é conhecida — solenoide com pulso cortado não completa o
> curso. Não havia muito a aprender gastando bancada para confirmar um resultado já
> esperado. Ver [02](02-arquitetura-eletrica-pinout.md#bypass-do-dimmerlink-para-psm-decidido).
>
> Se ainda quiser rodar a caracterização com o V2 (que está comprado), os números
> abaixo são o que medir — e o **mesmo procedimento serve para o PSM**, trocando
> "degraus de comando" por "duty da janela de semiciclos".

Os dados a extrair e o critério que era usado:

**Alvo de pré-infusão: 3–4 bar.** Não 2 bar. A faixa usada na prática é 2–4 bar, com
~3 bar como referência canônica (é a *line pressure* das máquinas comerciais
plumbadas, que fazem pré-infusão abrindo o grupo para a rede de água). Perfis de
*blooming* a 2 bar são a variante agressiva, não o padrão.

| Medir | Reprovado se |
|---|---|
| Nº de degraus inteiros entre **3 bar e 9 bar** | **< 15 degraus** — pouca autoridade para perfilar |
| Estabilidade da pressão em **3–4 bar** (pré-infusão) | oscilação/pulsação grosseira, ou a bomba alterna entre zumbir e jorrar |
| **Pressão mínima estável** que a bomba sustenta | **> 4 bar** — pré-infusão convencional fica fora de alcance |
| Repetibilidade subindo × descendo o comando | histerese grande |

*(Critério histórico: ✅ passou → seguia no DimmerLink; ❌ reprovou → migrava para
PSM. Superado pela decisão de 29/07/2026.)*

### Referência de campo

Relatos no [Home-Barista](https://www.home-barista.com/espresso-machines/successes-in-controlling-your-ulka-pump-for-pressure-profiling-t54317.html)
de quem controlou Ulka pelos dois métodos:

- **Dimmer (ângulo de fase):** posições marcadas em 3 / 6 / 9 bar funcionaram bem,
  com a bomba **travando abaixo de ~3 bar**.
- **Pulsando a bomba por microcontrolador:** funciona para pré-infusão, porém
  **mais barulhento**.

Ou seja: **~3 bar é o piso esperado do ângulo de fase**, o que coincide exatamente com
o limite inferior da faixa normal de pré-infusão. A leitura é moderadamente otimista —
o DimmerLink deve alcançar a pré-infusão convencional (3–4 bar) e ficar de fora só das
variantes mais agressivas.

> ⚠️ Aquele thread é de **2018**, anterior ao amadurecimento do Gaggiuino. Os dados
> empíricos valem; a conclusão de que "controle eletrônico de bomba vibratória
> continua sendo objetivo em aberto" envelheceu.

### O que pesar na decisão de migrar

Contras **reais** do PSM: **ripple de pressão** em duty baixo (pulsos espaçados) e a
volta da ISR de timing crítico ao firmware.

Ignore a afirmação de que PSM travaria ou queimaria bomba vibratória — é falsa, e
desmontada em [02](02-arquitetura-eletrica-pinout.md#-mito-psm-trava-e-queima-bomba-vibratória).

O ponto fraco previsível do que temos é a **pré-infusão**, não a extração: ângulo de
fase em potência baixa é onde bomba vibratória se comporta pior, porque o solenoide
não completa o curso. Extração a 9 bar deve ir bem em qualquer cenário.

> 🔄 **A recomendação de "não antecipar a migração" foi revertida em 29/07/2026.**
> Ela se apoiava em dois custos, e os dois encolheram:
>
> - **"Exige dessoldar em placa de rede AC"** — só se insistir no V2 comprado. O
>   **dimmer clássico expõe `Z-C` e `PWM` em header** por ~R$ 32, e é o que vamos
>   comprar. Zero solda em AC.
> - **"Devolve a ISR de timing crítico"** — devolve a ISR, mas **PSM tolera centenas
>   de µs de jitter**, contra dezenas do ângulo de fase, porque a decisão é binária e
>   o nível vem da contagem de pulsos. Com a ISR em IRAM, o risco é pequeno.

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
- [ ] **`MISO` do módulo microSD medido em alta impedância com o `CS` inativo** —
      fazer isto **antes** de montar o chicote, com multímetro
      ([02](02-arquitetura-eletrica-pinout.md#-2-o-miso-do-módulo-microsd-solta-o-barramento--falta-medir)).
      Módulo que não solta o barramento impede o MAX31865 de responder, e a falha só
      aparece com os dois juntos.
- [ ] microSD monta e grava (SPI compartilhado com o MAX31865 → conferir que ambos
      leem alternadamente sem se corromper).
- [ ] **Célula de carga:** tara com o prato vazio e calibração com **peso conhecido**
      (dois pontos). Conferir linearidade num terceiro peso — célula barata satura
      antes do nominal.
- [ ] **Célula de carga com a bomba ligada:** medir quanto a vibração de 60 Hz
      aparece na massa, com a balança montada como vai ficar. É o número que dita o
      filtro e a taxa de amostragem — e se for grande demais, o conserto é
      **mecânico** (desacoplar), não digital.
- [ ] Testar SSR e dimmer com **carga de teste** (lâmpada) antes da resistência/bomba reais.
- [ ] Validar proteções: simular sensor aberto (desconectar a sonda) → sistema deve cortar.

**Na máquina:**

- [ ] Termostato de vapor confirmado **em série** com o SSR (backstop) e fusível térmico intacto.
- [ ] Chassi aterrado.
- [ ] Sonda PT100 com pasta térmica e bem fixada — sonda solta é falha silenciosa.
- [ ] Primeiro aquecimento **supervisionado**, com termômetro independente conferindo, e
      setpoint conservador (ex.: 85 °C) antes de confiar na malha.
- [ ] Só então cargas reais em uso normal, com extintor por perto.
