# SmartFilco

Digitalização e automação completa de uma cafeteira espresso **Philco 15 Bar**
(idêntica à Philco 20 Bar) usando um **ESP32-S3** com display touch, controle
digital de temperatura (PID) e de pressão (malha fechada), logging de extração
e interface web opcional.

> Objetivo: transformar um "mod" comum de Philco em um projeto de engenharia
> reproduzível — no espírito do Gaggiuino, mas com arquitetura moderna
> (ESP32-S3, touchscreen, Wi-Fi, web UI, OTA).

---

## Estado atual do projeto

**Fase 1 — Documentação e definição de hardware: ✅ concluída**

- [x] Máquina identificada e caracterizada (Philco 15 Bar, single boiler, bomba vibratória)
- [x] Arquitetura elétrica definida (sensores, atuadores, MCU)
- [x] Esquema hidráulico definido
- [x] Display escolhido e pinagem real confirmada pelo esquemático oficial
- [x] BOM comprável dividida em 2 levas
- [ ] **Próximo:** abrir a máquina (engenharia reversa física) → ver [docs/06](docs/06-engenharia-reversa-checklist.md)
- [ ] Comprar Leva 1 (eletrônica) no AliExpress
- [ ] Comprar Leva 2 (hidráulica, em 1/8" NPT) após abrir a máquina

---

## Índice da documentação

| Doc | Conteúdo |
|-----|----------|
| [01 — Visão geral do projeto](docs/01-projeto-visao-geral.md) | Objetivo, escopo, como a máquina funciona, decisões de arquitetura |
| [02 — Arquitetura elétrica e pinagem](docs/02-arquitetura-eletrica-pinout.md) | Diagrama elétrico, pinagem real do ESP32-S3, lado AC |
| [03 — Esquema hidráulico](docs/03-esquema-hidraulico.md) | Circuito de água, pontos de sensor, OPV |
| [04 — BOM / Lista de compras](docs/04-bom-lista-de-compras.md) | Tudo a comprar, dividido em Leva 1 (eletrônica) e Leva 2 (hidráulica) |
| [05 — Arquitetura de firmware](docs/05-firmware-arquitetura.md) | Estrutura modular, controle, UI, web |
| [06 — Engenharia reversa (checklist)](docs/06-engenharia-reversa-checklist.md) | O que medir/anotar ao abrir a máquina |
| [07 — Segurança e calibração](docs/07-seguranca-e-calibracao.md) | Riscos de rede AC, aterramento, calibração dos sensores e da bomba |

---

## Resumo de 30 segundos

- **MCU + display:** Waveshare **ESP32-S3-Touch-LCD-3.5** (ST7796 SPI, ESP32-S3R8, 8MB PSRAM, RTC + microSD onboard).
- **Temperatura:** 2× **PT1000** classe A + 2× **MAX31865** (boiler + saída do grupo). Controle por **PID → SSR**.
- **Pressão:** 1× sensor **0–16 bar / 0,5–4,5V** lido por **ADS1115** (ADC por I²C — obrigatório, ver docs/02). Controle em **malha fechada → dimmer TRIAC** na bomba.
- **Atuadores:** **SSR-40DA** (resistência) e módulo **dimmer AC com zero-cross** (bomba vibratória).
- **Hidráulica:** padronizada em **1/8" NPT**, mangueira **4mm**. TEE + OPV ajustável + sensor de pressão + manômetro.
- **Segurança:** **fusível térmico original mantido**, fonte isolada Hi-Link, aterramento e isolação do lado AC.

⚠️ **Este projeto mexe com rede elétrica (127/220V) e água quente sob pressão.**
Leia [docs/07 — Segurança](docs/07-seguranca-e-calibracao.md) antes de qualquer coisa.
