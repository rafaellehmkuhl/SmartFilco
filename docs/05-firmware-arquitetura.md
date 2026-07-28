# 05 — Arquitetura de firmware

> Ainda não é fase de implementação — este documento fixa a **arquitetura alvo**
> para o firmware nascer organizado.

## Princípios

- **Modular**, orientado a responsabilidades. Cada periférico/serviço é um módulo isolado.
- Separação lógica entre **tempo-real/segurança** (leitura de sensores, PID, proteções)
  e **interface** (Wi-Fi, API, depois LVGL) — mesmo rodando num único ESP32-S3.
- **Segurança em primeiro lugar:** proteções por software independentes da UI.
- **Headless primeiro.** A primeira fase não tem display: a interface é web/serial.
  Isso é uma vantagem arquitetural — obriga a API a ser a fonte única de verdade, e
  o LVGL entra depois como **mais um cliente** dela, não como caminho paralelo.

## Divisão em duas "placas lógicas"

Embora fisicamente seja um único ESP32:

- **Control Board (tempo-real):** sensores, PID de temperatura, controle de pressão, atuadores, watchdog, limites de segurança.
- **UI Board (interface):** Wi-Fi, web server, API, logging — e, na fase de interface, o touchscreen (LVGL).

Na prática, sugestão de uso dos dois núcleos do ESP32-S3:
- **Core 0:** loop de controle determinístico (amostragem de sensores, PID, disparo da bomba, SSR, proteções).
- **Core 1:** Wi-Fi, web, gravação no microSD — e depois UI/LVGL.

## Módulos propostos

| Módulo | Responsabilidade | Fase |
|---|---|---|
| `TemperatureModule` | Lê o MAX31865 (SPI), checa o registrador de **fault**, aplica filtro, expõe `T_boiler` e `T_bolo_estimada` (offset modelado) | 1 |
| `PressureModule` | Lê o sensor via ADS1115 (I²C), converte 0,5–4,5 V → bar, **filtra a ondulação de 60 Hz da bomba**, aplica calibração | 1 |
| `PumpModule` | Controle da bomba via **DimmerLink** (UART 115200, `02 53 00 LEVEL`), **LUT própria** `pressão → comando 0–100` + PID de pressão (malha fechada). Interface abstrata o bastante para trocar por PSM sem mexer no resto | 1 |
| `BoilerModule` | PID de temperatura → SSR (slow PWM por janela de tempo) | 1 |
| `SafetyModule` | Limites de temperatura/pressão, timeout de bomba, sensor aberto/curto/travado, watchdog, estado seguro no boot | 1 |
| `LoggerModule` | Gravação de cada shot no microSD (CSV/JSON) + histórico | 1 |
| `WiFiModule` | Conexão à rede local, mDNS, **NTP** (substitui o RTC) | 1 |
| `WebServer` | UI web + API REST/WebSocket para gráficos em tempo real — **interface principal da fase 1** | 1 |
| `Storage` | Configurações persistentes (NVS + microSD) | 1 |
| `ShotModule` | Máquina de estados da extração: pré-infusão → ramp → hold → declínio; timer, integra fluxo (futuro) | 2 |
| `ProfileModule` | Perfis de pressão/temperatura, armazenados no microSD | 2 |
| `SteamModule` | Modo vapor (setpoint alto, teto pelo termostato de vapor em hardware) | 2 |
| `OTA` | Atualização de firmware pela rede | 2 |
| `DisplayModule` | UI LVGL — **cliente da mesma API do WebServer** | 3 |

> **`RTCModule` foi removido.** Sem o board Waveshare não há PCF85063; o timestamp
> de shot vem de **NTP** via Wi-Fi. Só voltaria a fazer sentido se houvesse
> requisito de log correto sem rede — não há.

## Loop de controle (essência)

```
a cada ~50–100 ms:
  T_boiler = TemperatureModule.readBoiler()
  P        = PressureModule.readBar()

  # temperatura
  duty_ssr = BoilerModule.pid(setpoint_T, T_boiler)
  SSR.apply(duty_ssr)                 # janela de tempo (slow PWM)

  # pressão (durante o shot)
  if shot.active:
    p_target = ProfileModule.targetAt(shot.elapsed)
    duty_pump = PumpModule.pid(p_target, P)   # malha fechada
    DimmerLink.set(duty_pump)                  # 3-4 bytes na UART; sem ISR

  SafetyModule.check(T_boiler, P, currents)
```

> Note que `T_group` **saiu do laço** — existe um sensor só. A temperatura da água
> no bolo é derivada por modelo de offset, não medida em operação
> ([07](07-seguranca-e-calibracao.md#caracterização-do-offset-boiler-bolo)).

## Interface

### Fase 1 — web (headless)

Página única servida pelo ESP32, WebSocket para telemetria ao vivo:

```
   93.5 °C  (bolo ≈ 89.2 °C est.)      8.9 bar
   ┌────────────────────────────────────────┐
   │  gráfico ao vivo P / T                 │
   └────────────────────────────────────────┘
   Shot 00:18                    histórico ▾
```

Controles: **Coffee · Steam · Flush · Settings** + edição de setpoint e perfil.

### Fase 3 — touch local

Mesma informação em LVGL num 3,5", consumindo a **mesma API**. Nenhuma lógica de
controle vive na camada de UI, em nenhuma das duas.

## Dados registrados por shot

`tempo, T_boiler, T_bolo_estimada, pressão, potência_resistência, potência_bomba, fluxo(futuro)`
→ gravados no microSD, visualizáveis em gráfico no navegador.

> Guardar sempre `T_boiler` **crua**, além da estimada. É o que permite refinar o
> modelo de offset depois, em cima do histórico, sem repetir a bancada.

## Stack sugerida

- **ESP-IDF** ou **Arduino-ESP32** (a definir na fase de implementação; ambos servem).
- Web: servidor HTTP + WebSocket para telemetria em tempo real; front simples embarcado no flash.
- **LVGL** só na fase 3 (driver ST7796 + touch FT6336 bem suportados).

## Cuidados específicos deste hardware

- **SPI compartilhado** entre MAX31865 (modo 1) e microSD (modo 0): todo acesso
  dentro de `beginTransaction` com o `SPISettings` correto por dispositivo. Acesso
  solto fora de transação é o bug clássico aqui.
- **Filtro na pressão** é obrigatório, não opcional: a bomba vibratória pulsa a
  60 Hz e sem filtro o PID de pressão persegue a ondulação.
- **Fault do MAX31865:** ler o registrador de falha, não inferir por valor absurdo.
- **Estado seguro no boot:** SSR e bomba desligados **antes** de qualquer
  inicialização de Wi-Fi ou UI. Para a bomba isso significa **mandar 0% ao
  DimmerLink explicitamente** — ele é um controlador independente e mantém o último
  valor recebido, inclusive através de um reset do ESP32.
- **Watchdog do lado do dimmer:** como o DimmerLink não sabe se o ESP32 morreu, o
  `SafetyModule` precisa reenviar 0% em qualquer caminho de falha. Um ESP32 travado
  com a bomba em 80% não para sozinho.
- **Nada de `RBDDimmer`:** essa biblioteca é do dimmer clássico com pinos ZC/PWM.
  Aqui o protocolo é serial cru — ver [github.com/robotdyn-dimmer/DimmerLink](https://github.com/robotdyn-dimmer/DimmerLink).
- **Sem ISR de zero-cross:** o DimmerLink cuida do timing. O laço de controle não
  tem mais nenhuma seção crítica de microssegundos.
- **`SET` não desgasta EEPROM** — pode ser chamado no laço de controle. Mas **nunca**
  emitir `0x02 0x5B` (SWITCH_I2C): desativa a UART permanentemente, gravado em EEPROM.
- **Comando é inteiro 0–100.** O PID interno pode trabalhar em ponto flutuante, mas
  o valor enviado é quantizado — evitar que a quantização vire *limit cycle*
  (oscilação em torno do setpoint saltando entre dois degraus). Zona morta ou
  histerese de 1 degrau na saída resolve.
- **`PumpModule` atrás de uma interface.** Se a caracterização reprovar o DimmerLink
  ([07](07-seguranca-e-calibracao.md)), a troca para PSM deve ser uma implementação
  nova do mesmo contrato — não uma refatoração do `ShotModule`.
