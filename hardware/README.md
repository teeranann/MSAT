# MSAT Hardware

Wiring and component information for building the MSAT device.

## Files
- `wiring/WIRING.md` — as-built connection table (power rails, level converter, pins) + part download links
- `wiring/msat-wiring.yml` — **WireViz source of truth** (pin-by-pin harness spec)
- `wiring/msat-wiring.html` — interactive harness diagram + BOM (open in browser)
- `wiring/msat-wiring.svg` / `.png` — rendered harness diagram (paper-grade)
- `wiring/msat-wiring.bom.tsv` — bill of materials (drops into Excel)
- `wiring/msat-wiring-schematic.svg` — high-level block diagram (overview figure)
- `wiring/msat-wiring-components.xlsx` — full wiring list and 30-line purchased BOM (same as Supplementary Tables S1 and S2)
- `wiring/msat-wiring.fzz` — Fritzing breadboard/schematic sketch (work-in-progress)
- `wiring/fritzing-parts/*.fzpz` — custom Fritzing parts
- `3d-print/msat-probe-lid.stl` — probe-mounting lid for the titration beaker (PETG)
- `3d-print/README.md` — print settings and material notes

To re-render the harness after editing the YAML:
```
pip install wireviz   # plus a system install of Graphviz (graphviz.org / winget install Graphviz.Graphviz)
wireviz wiring/msat-wiring.yml
```

## Main components
| Subsystem | Part | Interface |
|---|---|---|
| MCU | ESP32‑WROOM dev board | — |
| Power | 12 V / 5 A supply, LM2596 #1 (5 V bus), LM2596 #2 (pump, ~7.5 V) | — |
| pH | E‑201‑C electrode + analog interface board → ADS1115 16‑bit ADC (5 V) | I²C (0x48) via LLC‑4CH‑I2C level converter |
| EC | MI‑Water‑EC485 platinum‑black electrode + transmitter (12 V) | RS‑485 / Modbus via MAX485 |
| Colour | TCS34725 breakout (3.3 V) | I²C (0x29) |
| Temperature | DS18B20 probe | 1‑Wire (GPIO4) |
| Titrant mass | 200 g load cell + HX711 | GPIO25/26 |
| Pump | 12 V / 3 W peristaltic pump, relay‑switched | GPIO13 |
| Clock | DS3231 RTC (3.3 V) | I²C (0x68) |
| Display | 16×2 LCD, PCF8574 backpack (5 V) | I²C (0x27) |
| Storage | microSD | SPI (CS GPIO5) |

See [`../firmware/README.md`](../firmware/README.md) for the full pin map and
flashing notes (mind the GPIO2/GPIO15 strapping pins).

## Calibration
- **pH:** three‑point (pH 4.01 / 6.86 / 9.18 buffers, as stored in the firmware)
- **EC:** KCl standards across the working range
- **Load cell:** known‑mass calibration

---
Hardware design © 2026 Burapha University · CERN‑OHL‑S v2 · Patent pending No. 2603001145
