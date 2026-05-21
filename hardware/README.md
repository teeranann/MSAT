# MSAT Hardware

Wiring and component information for building the MSAT device.

## Files
- `wiring/Autotitration-Components-Wiring-*.xlsx` — full component & wiring list
- `wiring/autotitration.fzz` — Fritzing breadboard/schematic sketch
- `wiring/Frizting modules/*.fzpz` — custom Fritzing parts (ADS1115, DS18B20,
  GY‑33, WAGO connectors)

## Main components
| Subsystem | Part | Interface |
|---|---|---|
| MCU | ESP32‑WROOM dev board | — |
| pH | electrode + ADS1115 16‑bit ADC | I²C (0x48) |
| EC | conductivity meter | RS‑485 / Modbus |
| Colour | GY‑33 / TCS34725 | I²C (0x29) |
| Temperature | DS18B20 probe | 1‑Wire (GPIO4) |
| Titrant mass | load cell + HX711 | GPIO25/26 |
| Pump | relay‑driven dosing pump | GPIO13 |
| Clock | DS3231 RTC | I²C (0x68) |
| Display | 16×2 LCD | I²C (0x27) |
| Storage | microSD | SPI (CS GPIO5) |

See [`../firmware/README.md`](../firmware/README.md) for the full pin map and
flashing notes (mind the GPIO2/GPIO15 strapping pins).

## Calibration
- **pH:** two‑point (e.g. pH 4.01 / 9.18 buffers)
- **EC:** KCl standards across the working range
- **Load cell:** known‑mass calibration

---
Hardware design © 2026 Burapha University · CC BY‑NC 4.0 · Patent pending No. 2603001145
