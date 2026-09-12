# MSAT Hardware

Wiring and component information for building the MSAT device.

## Files
- `wiring/WIRING.md` — firmware‑exact connection table + part download links
- `wiring/msat-wiring.yml` — **WireViz source of truth** (pin-by-pin harness spec)
- `wiring/msat-wiring.html` — interactive harness diagram + BOM (open in browser)
- `wiring/msat-wiring.svg` / `.png` — rendered harness diagram (paper-grade)
- `wiring/msat-wiring.bom.tsv` — bill of materials (drops into Excel)
- `wiring/msat-wiring-schematic.svg` — high-level block diagram (overview figure)
- `wiring/msat-wiring-components.xlsx` — full component & wiring list
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
| pH | electrode + ADS1115 16‑bit ADC | I²C (0x48) |
| EC | conductivity meter | RS‑485 / Modbus |
| Colour | TCS34725 breakout | I²C (0x29) |
| Temperature | DS18B20 probe | 1‑Wire (GPIO4) |
| Titrant mass | load cell + HX711 | GPIO25/26 |
| Pump | relay‑driven dosing pump | GPIO13 |
| Clock | DS3231 RTC | I²C (0x68) |
| Display | 16×2 LCD | I²C (0x27) |
| Storage | microSD | SPI (CS GPIO5) |

See [`../firmware/README.md`](../firmware/README.md) for the full pin map and
flashing notes (mind the GPIO2/GPIO15 strapping pins).

## Calibration
- **pH:** three‑point (pH 4.01 / 6.86 / 9.18 buffers, as stored in the firmware)
- **EC:** KCl standards across the working range
- **Load cell:** known‑mass calibration

---
Hardware design © 2026 Burapha University · CERN‑OHL‑S v2 · Patent pending No. 2603001145
