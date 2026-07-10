# 07 — Segurança e calibração

## ⚠️ Segurança elétrica (rede AC 127/220V)

Este projeto trabalha com **tensão de rede** e **água quente sob pressão**. Erros
podem causar choque, incêndio ou queimadura.

- **Sempre desligar da tomada** antes de mexer. Não confie só na chave.
- **Manter o fusível térmico original** do boiler — é a última proteção contra
  superaquecimento se o controle falhar. **Nunca** remover ou "pontear".
- **Isolação:** todo o lado de baixa tensão (ESP32, ADS1115, MAX31865) é isolado do
  AC pelos **optoacopladores** do SSR e do dimmer e pela **fonte isolada Hi-Link**.
  Não criar caminhos que quebrem essa isolação.
- **Aterramento:** chassi metálico e partes condutoras **aterrados**. Água + eletricidade exige terra.
- **Dimensionamento:** SSR-40DA folgado para a resistência; fios de potência AWG adequado; usar Faston isolados.
- **Separação física:** manter a eletrônica de sinal longe do boiler e dos fios de potência; roteirizar AC e sinal separados.
- **Proteções por software (SafetyModule), independentes da UI:**
  - Corte se `T_boiler` > limite (ex.: 160°C) ou sensor **aberto/curto** (MAX31865 detecta falha).
  - **Timeout da bomba** (não bombear a seco / tempo máximo de shot).
  - Limite de **pressão** máxima.
  - **Watchdog** reiniciando o loop de controle se travar.
  - Estado seguro no boot e em falha: **SSR e bomba desligados**.

## Calibração dos sensores

### Temperatura (PT1000 + MAX31865)

- Configurar o MAX31865 para **PT1000** (Rref ≈ 4300Ω nas placas de PT1000) e para o
  número de fios da sonda (2/3/4 fios — jumper na placa).
- Verificar com referência conhecida (água em ebulição ~100°C no nível do mar, gelo ~0°C)
  e aplicar **offset** se necessário.
- **PT1000 #2 (grupo)** serve para medir o *offset* real entre boiler e água no café
  (o boiler pode marcar 95°C e a água sair ~89°C).

### Pressão (sensor 0,5–4,5V via ADS1115)

- Curva teórica: `P(bar) = (Vout − 0,5) / (4,5 − 0,5) × 16`.
- Calibrar contra o **manômetro analógico** (mesmo ponto no TEE): anotar pares
  (V lido, bar no manômetro) e ajustar ganho/offset.
- `0,5V → 0 bar`, `4,5V → 16 bar` são nominais; medir os reais.

## Calibração e controle da bomba (o ponto mais delicado)

### Por que é difícil

Bombas vibratórias (ULKA e clones) têm resposta **muito não-linear**: 50% de
potência **não** dá 50% da pressão. Por isso o dimmer manual atual não faz ajuste
fino e "trava" em ~11–12 bar sem chegar aos 9 bar desejados.

### Método de controle — a decidir após caracterizar

1. **Controle por ângulo de fase (TRIAC):** o que o dimmer faz. Simples, mas reduz
   torque e pode fazer a bomba parar de bombear antes do ponto desejado.
2. **Burst firing / ciclos inteiros:** liga/desliga ciclos completos da rede,
   sincronizado com o zero-cross. Costuma dar **estabilidade melhor** em bombas
   vibratórias. Vale testar.
3. **Malha fechada (recomendado):** independentemente do método de disparo, fechar
   o laço `pressão → PID → bomba` corrige a não-linearidade automaticamente. É o que
   permite *pressure profiling* real (2 bar de pré-infusão → 9 bar → declínio).

### Procedimento de caracterização (na bancada, com a máquina instrumentada)

1. Com o sensor de pressão já lendo, variar a potência da bomba em degraus (ex.: 10%→100%).
2. Registrar **pressão × potência** (com porta-filtro cego / restrição conhecida).
3. Montar uma **tabela de calibração** (feedforward) e usá-la como ponto de partida do PID.
4. Ajustar ganhos do PID de pressão para resposta estável, sem oscilação.

### Interação com a OPV

- Se a OPV abrir cedo, ela limita a pressão máxima e **atrapalha** o profiling.
- Estratégia: regular a OPV como **teto de segurança** (ex.: ~11–12 bar) e deixar o
  perfil de pressão por conta do controle eletrônico da bomba.
- Confirmar na engenharia reversa se **já existe** OPV e sua pressão ([06](06-engenharia-reversa-checklist.md)).

## Checklist de "primeiro ligamento" seguro

- [ ] Bancada: alimentar só a eletrônica (Hi-Link) **sem** conectar resistência/bomba ao AC.
- [ ] I²C scan OK (ADS1115 em 0x48).
- [ ] Leituras de temperatura coerentes (ambiente) e pressão em ~0 bar.
- [ ] Testar SSR e dimmer com **carga de teste** (lâmpada) antes da resistência/bomba reais.
- [ ] Validar proteções (simular sensor aberto → sistema deve cortar).
- [ ] Só então conectar as cargas reais, com extintor por perto e supervisão.
