# 05 — Arquitetura de firmware

> Ainda não é fase de implementação — este documento fixa a **arquitetura alvo**
> para o firmware nascer organizado.

## Princípios

- **Modular**, orientado a responsabilidades. Cada periférico/serviço é um módulo isolado.
- Separação lógica entre **tempo-real/segurança** (leitura de sensores, PID, proteções)
  e **interface** (LVGL, Wi-Fi, API) — mesmo rodando num único ESP32-S3.
- **Segurança em primeiro lugar:** proteções por software independentes da UI.

## Divisão em duas "placas lógicas"

Embora fisicamente seja um único ESP32:

- **Control Board (tempo-real):** sensores, PID de temperatura, controle de pressão, atuadores, watchdog, limites de segurança.
- **UI Board (interface):** touchscreen (LVGL), Wi-Fi, web server, API, logging.

Na prática, sugestão de uso dos dois núcleos do ESP32-S3:
- **Core 0:** loop de controle determinístico (amostragem de sensores, PID, zero-cross/TRIAC, SSR, proteções).
- **Core 1:** UI/LVGL, Wi-Fi, web, gravação no microSD.

## Módulos propostos

| Módulo | Responsabilidade |
|---|---|
| `TemperatureModule` | Lê os 2× MAX31865 (SPI), aplica filtro, expõe temperaturas boiler/grupo |
| `PressureModule` | Lê o sensor via ADS1115 (I²C), converte 0,5–4,5V → bar, aplica calibração |
| `PumpModule` | Controle da bomba: zero-cross + TRIAC, tabela de calibração + PID de pressão (malha fechada) |
| `BoilerModule` | PID de temperatura → SSR |
| `SteamModule` | Modo vapor (setpoint alto) |
| `ShotModule` | Máquina de estados da extração: pré-infusão → ramp → hold → declínio; timer, integra fluxo (futuro) |
| `ProfileModule` | Perfis de pressão/temperatura (pré-infusão, *pressure profiling*), armazenados no microSD |
| `SafetyModule` | Fusível térmico (respeitar), limites de temperatura/pressão, timeout de bomba, detecção de sensor aberto/curto, watchdog |
| `DisplayModule` | UI LVGL (telas e navegação) |
| `WiFiModule` | Conexão à rede local, mDNS |
| `WebServer` | UI web + API REST/WebSocket para gráficos em tempo real |
| `LoggerModule` | Gravação de cada shot no microSD (CSV/JSON) + histórico |
| `OTA` | Atualização de firmware pela rede |
| `Storage` | Configurações persistentes (NVS + microSD) |
| `RTCModule` | PCF85063 — timestamp dos shots |

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
    Triac.apply(duty_pump)                     # sincronizado com zero-cross

  SafetyModule.check(T_boiler, T_group, P, currents)
```

## Interface (touch)

Tela principal (estilo Decent, adaptada ao 3,5"):

```
   93.5 °C          8.9 bar
   ┌───────────────────────┐
   │  gráfico ao vivo P/T   │
   └───────────────────────┘
   Shot 00:18      Flow 2.1 ml/s
```

Botões: **Coffee · Steam · Flush · Backflush · Settings**.

Tela de perfil: pré-infusão (bar, s) → ramp → hold → declínio.

## Dados registrados por shot

`tempo, T_boiler, T_grupo, pressão, potência_resistência, potência_bomba, fluxo(futuro)`
→ gravados no microSD, visualizáveis em gráfico no navegador.

## Stack sugerida

- **LVGL** para UI (driver ST7796 + touch FT6336 bem suportados).
- **ESP-IDF** ou **Arduino-ESP32** (a definir na fase de implementação; ambos servem).
- Web: servidor HTTP + WebSocket para telemetria em tempo real; front simples embarcado no flash.
