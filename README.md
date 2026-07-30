# SmartFilco

Digitalização e automação completa de uma cafeteira espresso **Philco 15 Bar**
(idêntica à Philco 20 Bar) usando um **ESP32-S3**: controle digital de temperatura
(PID) e de pressão (malha fechada), logging de extração e interface web.

> Objetivo: transformar um "mod" comum de Philco em um projeto de engenharia
> reproduzível — no espírito do Gaggiuino, mas com arquitetura moderna
> (ESP32-S3, Wi-Fi, web UI, OTA, e touchscreen numa fase posterior).

---

## Estado atual do projeto

**Fase 1 — Documentação e definição de hardware: ✅ concluída**
**Leva 1 (eletrônica): ✅ comprada** — R$ 201,94 no AliExpress + itens já em mãos

- [x] Máquina identificada e caracterizada (Philco 15 Bar, single boiler, bomba vibratória)
- [x] Arquitetura elétrica definida (sensores, atuadores, MCU)
- [x] Esquema hidráulico definido
- [x] BOM da Leva 1 comprada e conferida contra o projeto
- [ ] **Próximo:** bancada headless (ESP32 + sensores na mesa) — não depende de abrir a máquina
- [ ] Abrir a máquina (engenharia reversa física) → ver [docs/06](docs/06-engenharia-reversa-checklist.md)
- [ ] Comprar Leva 2 (hidráulica, em 1/8" NPT) após abrir a máquina

### Revisões de projeto (jul/2026)

Quatro decisões mudaram depois de conferir a compra real contra a documentação:

| Antes | Agora | Por quê |
|---|---|---|
| Board Waveshare com touch | **ESP32-S3 devkit, headless** | já em mãos; display vira fase 3 e para de bloquear o início. RTC → NTP, SD → módulo SPI |
| PT1000 × 2 | **PT100 3 fios × 1** | a vantagem do PT1000 some com ligação a 3 fios, e a alegação de "maior resolução" no doc antigo **estava errada** |
| 2º sensor na saída do grupo | **cortado** | nenhum projeto DIY usa dois; a Philco não tem grupo para instrumentar. Offset vira caracterização única com termopar K |
| SSR direto na resistência | **SSR + termostato de vapor em série** | backstop de hardware contra SSR em curto / firmware travado, antes do fusível térmico |

Justificativas completas em [docs/01](docs/01-projeto-visao-geral.md#decisões-de-arquitetura-e-o-porquê).

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

## Código

| Onde | O quê |
|---|---|
| [`firmware/bringup/`](firmware/bringup/) | Prova de vida da placa em ESP-IDF: RGB, Wi-Fi e servidor web. Sem lógica de café |

---

## Resumo de 30 segundos

- **MCU:** **ESP32-S3-WROOM-1 N16R8** (dual USB-C), headless na fase 1 — interface via **Wi-Fi/web**. Timestamp por **NTP**; logging em **módulo microSD SPI**. ⚠️ PSRAM octal: GPIO33–37 são armadilha (estão no header, não funcionam).
- **Temperatura:** 1× **PT100** classe A, **3 fios**, de superfície, na parede do boiler + 1× **MAX31865** (Rref **430 Ω**). Controle por **PID → SSR**. Temperatura no bolo é **modelada**, não medida ([por quê](docs/01-projeto-visao-geral.md#por-que-um-sensor-de-temperatura-e-não-dois)).
- **Pressão:** 1× sensor **0–13,8 bar (200 PSI) / 0,5–4,5 V**, 1/8" NPT, lido por **ADS1115 em 5 V** (PGA ±6,144 V) através de **level shifter**. Controle em **malha fechada → dimmer** na bomba.
- **Atuadores:** **SSR-40DA + dissipador** (resistência) e **dimmer RobotDyn 8A por PSM** (bomba vibratória) — `Z-C` e `Dim` direto no ESP32, com a ISR de zero-cross em **IRAM**. Ângulo de fase foi descartado: em potência baixa entrega curso incompleto e a bomba zumbe sem bombear. Ver [docs/02](docs/02-arquitetura-eletrica-pinout.md#bypass-do-dimmerlink-para-psm-decidido).
- **Hidráulica:** padronizada em **1/8" NPT**, mangueira **4mm**. TEE + OPV ajustável + sensor de pressão + manômetro.
- **Segurança:** **fusível térmico original intacto**, **termostato de vapor em série com o SSR** como backstop de hardware, fonte isolada Hi-Link, aterramento e isolação do lado AC.

⚠️ **Este projeto mexe com rede elétrica (127/220V) e água quente sob pressão.**
Leia [docs/07 — Segurança](docs/07-seguranca-e-calibracao.md) antes de qualquer coisa.

⚠️ **Antes de abrir a máquina**, leia a seção 0 de
[docs/06](docs/06-engenharia-reversa-checklist.md) sobre o **parafuso do chuveiro** —
é o defeito que mais destrói essa máquina, e espaná-lo custa uma caldeira nova.
