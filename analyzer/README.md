# MSAT Data Analyzer

Desktop app (Python + Tkinter/ttkbootstrap + matplotlib) to analyze MSAT runs.

## Install & run
```bash
pip install -r requirements.txt
python msatdataanalyzer.py
```
Python 3.10+ recommended. `tkinterdnd2` is optional (drag‑and‑drop);
`websocket-client` enables the Live Monitor.

## What it does
- **Load a run** (`.txt`) via *Local File* or drag‑and‑drop; one‑shot load.
- **Derived curves:** pH (2‑point calibrated), colour ΔE (CIELAB or RGB),
  EC (auto µS/mS), temperature — plus 1st/2nd derivatives.
- **Equivalence points:** per‑sensor strategy (1st deriv. / 2nd deriv. /
  inflection trendlines); ΔT (enthalpy proxy).
- **Data processing:** auto "hiccup" despike (X‑axis glitch removal),
  Savitzky–Golay smoothing, click‑to‑delete a point, manual exclude.
- **Display:** per‑axis ranges, tick interval, point size/font, X in mL/mol/sec/g.
- **Live Monitor:** *Check MSAT* → *Live Monitor* listens to the device
  WebSocket (`ws://192.168.1.200/ws`) and plots the run in real time
  (seamless across multiple runs; UI locks until you Stop or load a file).
- **Export:** PNG/TIFF/SVG/PDF (filenames prefixed `-`) plus a
  `-<name>-config.txt` sidecar that restores all settings when the file is
  reopened.

## Notes
- Device default IP is `192.168.1.200` (edit `self.msat_ip` if changed).
- The high‑resolution record is the device SD file; Live Monitor is a coarse
  real‑time preview — sync the full file afterwards for detailed analysis.

---
Copyright © 2026 Burapha University · PolyForm Noncommercial 1.0.0 · Patent pending No. 2603001145
