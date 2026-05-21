# Sample data

One representative titration run per acid (NaOH titrant). Load any `.txt` in
the Analyzer (`analyzer/`).

## File format (CSV)
Header row, then one row per sample (~0.25–0.5 s apart):
```
SampleIndex,Timestamp,Temp,pH,Volt,EC,Weight,Weightloss,R,G,B,C,RelayStatus
```
| Column | Meaning | Unit |
|---|---|---|
| Temp | temperature | °C |
| pH | electrode pH | — |
| Volt | pH electrode voltage | V |
| EC | conductivity | µS/cm |
| Weight / Weightloss | balance reading / titrant delivered | g |
| R,G,B,C | colour sensor channels | raw |
| RelayStatus | pump ON/OFF | — |

`Weightloss` (g) is the titrant axis (≈ mL at density 1.0).

## Acids included
KHP, HCl, H₂SO₄, oxalic, phosphoric, malic, citric, succinic, benzoic.

---
Data © 2026 Burapha University · CC BY‑NC 4.0
