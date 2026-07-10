# 04 — BOM / Lista de compras

Dividida em **duas levas**:

- **Leva 1 — Eletrônica:** comprar **agora** (não depende de abrir a máquina).
- **Leva 2 — Hidráulica:** comprar **depois** da engenharia reversa ([06](06-engenharia-reversa-checklist.md)), quando confirmar rosca/espaço/OPV existente. Como o frete leva ~2 semanas, você monta a bancada eletrônica em paralelo.

> Decisão do usuário: **sem redundância** (1 de cada). Risco assumido: se o sensor de
> pressão ou uma PT1000 vier com defeito, são +2 semanas de espera. O único item onde
> ainda vale considerar 1 reserva é o **sensor de pressão** (o mais chato de trocar com
> a máquina fechada).

---

## 🛒 Leva 1 — Eletrônica (comprar agora)

### Controle / lógica

| Qtd | Item | Buscar por | Notas |
|---|---|---|---|
| 1 | Waveshare **ESP32-S3-Touch-LCD-3.5** (variante **ST7796/SPI**) | "ESP32-S3-Touch-LCD-3.5" | ⚠️ pegar a versão **SPI (ST7796)**, não a QSPI (AXS15231B) |
| 1 | Cartão **microSD** 8–32 GB | qualquer marca decente | logging dos shots |
| 1 | **ADS1115** 16-bit I²C | "ADS1115 module" | **obrigatório** (não há ADC livre no board) |
| 2 | Módulo **MAX31865** — versão **PT1000** | "MAX31865 PT1000" | ⚠️ existe versão PT100; pegar **PT1000** (Rref 4300Ω) |

### Sensores

| Qtd | Item | Buscar por | Notas |
|---|---|---|---|
| 2 | **PT1000 classe A**, sonda inox rosqueada | "PT1000 class A stainless threaded probe" | boiler + grupo |
| 1 | Sensor de **pressão 0–16 bar** (1,6 MPa), saída **0,5–4,5V** | "pressure sensor 1.6MPa 0.5-4.5V" | ver rosca na Leva 2; considerar 1 reserva |

### Atuadores / potência

| Qtd | Item | Buscar por | Notas |
|---|---|---|---|
| 1 | **SSR-40DA** (DC-in / AC-out), marca decente | "SSR-40DA Omron" / genuíno | **evitar Fotek falsificado de R$10** |
| 1 | Módulo **dimmer AC** com **zero-cross + TRIAC** isolado | "RobotDyn AC dimmer 1 channel" | controle da bomba; caracterizar antes de decidir método |
| 1 | Fonte isolada **HLK-5M05** (AC→5V, ~5W) | "HLK-5M05" | alimenta ESP32 + módulos |
| 1 | (opcional) DC-DC 5V→3,3V | "MP1584" | só se algum módulo exigir 3,3V próprio |

### Fiação / mecânica

| Item | Notas |
|---|---|
| Cabo de silicone AWG18–20 resistente a temperatura | ligações de sinal e potência |
| Terminais **Faston**, ilhoses, tubo **termo-retrátil** | conexões na máquina |
| Bornes **WAGO** (ou equivalentes) | distribuição e junções |
| Parafusos **M3**, **inserts de latão** (heat-set), espaçadores de nylon | fixação dos módulos na peça 3D |
| Filamento (PETG recomendado pela temperatura) | suporte impresso dos módulos |

### Futuro (não precisa agora, mas já deixar previsto)

| Qtd | Item | Buscar por | Função |
|---|---|---|---|
| 1 | Flow meter Hall | "coffee flow meter hall" / "YF-S401" | receita por volume |
| 2 | **ACS712** (20A) | "ACS712 20A module" | diagnóstico de corrente (resistência/bomba) |
| 1 | Sensor de nível (boia magnética) | "magnetic float switch" | evitar bomba a seco |

---

## 🔧 Leva 2 — Hidráulica (comprar após abrir a máquina)

Padronizar tudo em **1/8" NPT** + mangueira **4 mm**.

| Qtd | Item | Notas |
|---|---|---|
| 1 | **TEE 1/8" NPT** | ponto de pressão: manômetro + sensor |
| 1 | **OPV ajustável** para espresso | confirmar entrada/saída 1/8" NPT; regular como teto de segurança |
| 1 | **Adaptador BSP→NPT** (seguro) | caso o sensor de pressão só exista em G1/8 |
| — | Adaptadores **1/8" NPT → espigão/push-fit 4 mm** | conexão às mangueiras |
| — | Mangueira de silicone/PTFE alta temperatura, 4 mm | comprimento conforme a máquina |
| — | **Fita veda-rosca PTFE** | vedação das roscas NPT |
| 1 | (se não existir) manômetro 0–16 bar 1/8" NPT | você já tem um de 12 bar; avaliar trocar por 16 bar |

> **Antes de fechar a Leva 2, confirme na máquina:** se já há OPV (e sua pressão),
> macho/fêmea de cada rosca, espaço físico para TEE+OPV+sensor+manômetro, e o
> comprimento de mangueira. Checklist completo em [06](06-engenharia-reversa-checklist.md).

---

## Itens que saíram da lista (já vêm no board)

- **RTC** → PCF85063 onboard.
- **Armazenamento para logging** → slot microSD onboard.
- **ESP32-S3 DevKit separado** → o próprio board já é o MCU.
