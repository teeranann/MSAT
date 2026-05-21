# MSAT — Multi-Sensor Automatic Titrator

A low-cost, **ESP32-based open-hardware automatic acid–base titrator** with
gravimetric titrant delivery. It records **pH, electrical conductivity (EC),
colour change (ΔE), and temperature** simultaneously and in real time, serves a
live **WiFi dashboard**, and logs every run to an SD card. A companion desktop
**data analyzer** finds equivalence points (1st/2nd derivative + inflection
trendlines) and estimates titration enthalpy, and a **sync tool** copies runs
from the device to a PC.

> Copyright © 2026 **Burapha University**. Inventor/developer: **Teeranan Nongnual**.
> **Petty Patent pending — Application No. 2603001145** (filed 2026‑05‑12).
> Noncommercial use only — see [License](#license).

---

## Why MSAT?

Conventional manual burettes give discrete points and rely on subjective visual
endpoints. MSAT delivers titrant by mass (high precision), captures four sensor
channels continuously, and determines endpoints objectively across mono- and
polyprotic acid–base systems — at a fraction of the cost of commercial
autotitrators, suitable for teaching and research labs.

## Repository layout

| Folder | What it is |
|--------|------------|
| [`firmware/`](firmware/) | ESP32 Arduino firmware (`.ino`) — sensor acquisition, filtering, WiFi dashboard, SD logging |
| [`analyzer/`](analyzer/) | Python desktop app — load/analyze runs, EP detection, ΔT, live monitor |
| [`sync/`](sync/) | Windows + Git Bash tool — download runs from the device, clean up, protect folder |
| [`hardware/`](hardware/) | Wiring diagram, Fritzing sketch, component list |
| [`sample-data/`](sample-data/) | Example titration runs (one per acid) |
| [`docs/`](docs/) | Images and additional documentation |

## Quick start

1. **Build the device** — see [`hardware/`](hardware/) for wiring (ESP32 + ADS1115 pH,
   RS‑485/Modbus EC meter, GY‑33 colour, DS18B20 temperature, HX711 load cell, relay pump).
2. **Flash the firmware** — open [`firmware/MSAT-Autotitration/MSAT-Autotitration.ino`](firmware/),
   set your WiFi name/password (lines marked `EDIT ME`), install the required
   libraries, and upload. See [`firmware/README.md`](firmware/README.md).
3. **Open the dashboard** — browse to the device IP (default `192.168.1.200`) to
   start/monitor a titration live.
4. **Sync the data** — run [`sync/runmsatsync.bat`](sync/) to copy runs to your PC.
5. **Analyze** — `pip install -r analyzer/requirements.txt` then
   `python analyzer/msatdataanalyzer.py`. See [`analyzer/README.md`](analyzer/README.md).

## Key features

- **4 sensors in one run:** pH (ADS1115), EC (Modbus meter), colour ΔE (CIELAB/RGB), temperature (DS18B20)
- **Gravimetric titrant delivery** via load cell — endpoint by volume *or* mass
- **Real-time WiFi dashboard** (WebSocket) + on-device LCD
- **Auto-stop** by weight loss or time; SD logging of every run
- **Robust signal conditioning** (median + EMA + anomaly rejection) — clean curves, sharp inflections
- **Analyzer:** equivalence points (derivative + inflection trendlines), ΔT/enthalpy, glitch despike, live monitor via device WebSocket, batch export (PNG/TIFF/SVG/PDF) and config save/restore
- **Sync tool:** fast WiFi download, "clean up space" on the device, read-only folder protection

## License

This is **noncommercial open hardware/software** (not OSI "open source" — commercial use is reserved):

- **Software** (firmware, analyzer, sync): **PolyForm Noncommercial 1.0.0** — see [`LICENSE`](LICENSE)
- **Documentation, hardware files, images, sample data:** **CC BY‑NC 4.0**

You may **use, study, modify, and share** for **noncommercial** purposes
(education, research, schools, universities) **with attribution**. **Selling the
system or code, or any commercial use, is prohibited** without written
permission; patent rights are reserved by Burapha University. See [`NOTICE`](NOTICE).
For commercial licensing, contact Teeranan Nongnual <teeranan.no@buu.ac.th>.

## Citation

If you use MSAT in academic work, please cite it — see [`CITATION.cff`](CITATION.cff)
(a related manuscript is in preparation; cite the paper too once published).
