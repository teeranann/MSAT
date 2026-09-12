# Sample data

One representative titration run per acid (NaOH titrant). Load any `.txt` in
the Analyzer (`analyzer/`).

**These nine files are the exact runs the figures in the accompanying manuscript
were produced from**, so every published trace can be regenerated from this
folder.

| Acid | Run file | Figure panel source |
|---|---|---|
| KHP | `20260519_095745_KHP-3.txt` | `XO-20260519_095745_KHP-3` |
| HCl | `20260524_222410_HCl-12.txt` | `XO-20260524_222410_HCl-12` |
| H₂SO₄ | `20260524_224718_H2SO4-11.txt` | `XO-20260524_224718_H2SO4-11` |
| Oxalic | `20260525_145642_oxalic-12.txt` | `XO-20260525_145642_oxalic-12` |
| Phosphoric | `20260524_220156_phosphoric-15.txt` | `XO-20260524_220156_phosphoric-15` |
| Malic | `20260525_141012_malic-12.txt` | `XO-20260525_141012_malic-12` |
| Citric | `20260519_162855_Citric-12.txt` | `XO-20260519_162855_Citric-12` |
| Succinic | `20260521_102418_Succinic-17.txt` | `XO-20260521_102418_Succinic-17` |
| Benzoic | `20260521_154031_Benzoic-13.txt` | `XO-20260521_154031_Benzoic-13` |

All nine were recorded on the released firmware at a logging interval of
500 ms (2 Hz) and use the 13-column schema below.

## File format (CSV)
Header row, then one row per sample (500 ms apart):
```
SampleIndex,Timestamp,Temp,pH,Volt,EC,Weight,Weightloss,R,G,B,C,RelayStatus
```
| Column | Meaning | Unit |
|---|---|---|
| Temp | temperature | °C |
| pH | electrode pH | — |
| Volt | pH electrode voltage (amplified module output) | V |
| EC | conductivity | µS/cm |
| Weight / Weightloss | balance reading / titrant delivered | g |
| R,G,B,C | colour sensor channels | raw counts |
| RelayStatus | pump ON/OFF | — |

`Weightloss` (g) is the titrant axis (≈ mL at density 1.0).

Mass is recorded to two decimals, so the recording quantum is 10 mg; the
short-term repeatability of the mass channel in these runs is 5–6 mg.

pH is obtained by piecewise-linear interpolation between the three calibration
buffers (4.01 / 6.86 / 9.18), with linear extrapolation of the terminal segment
outside that range and a hard clamp to 0–14. Values above pH 9.18 are therefore
extrapolated.

## Acids included
KHP, HCl, H₂SO₄, oxalic, phosphoric, malic, citric, succinic, benzoic.

---
Data © 2026 Burapha University · CC BY 4.0
