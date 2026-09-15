# MSAT Wiring (as built)

Connection spec for the instrument that produced the published data. Pin numbers
are ESP32 GPIO numbers and match the `#define`s in
[`../../firmware/msat-firmware/msat-firmware.ino`](../../firmware/msat-firmware/msat-firmware.ino);
supply rails and the I²C level converter are taken from the built unit, which the
firmware cannot show. This file, `msat-wiring.yml` and
`msat-wiring-components.xlsx` agree with Supplementary Table S2 of the paper.

> ⚠️ **Strapping pins:** GPIO2 (MAX485 DE/RE) and GPIO15 (Button 3) are ESP32
> strapping pins — disconnect them while flashing, then reconnect.

## Power
| Source | Feeds |
|---|---|
| 12 V / 5 A (60 W) switching supply | LM2596 #1, LM2596 #2, MI-Water-EC485 EC transmitter (red V+, black GND) |
| LM2596 #1 — **5 V bus** (lock at 5 V) | ESP32 VIN, ADS1115, pH interface board, HX711, MAX485, microSD module, relay module, LCD, level-converter HV side |
| LM2596 #2 — **pump supply** (~7.5 V, sets the flow rate) | relay COM → pump |
| ESP32 on-board regulator — **3.3 V bus** | DS3231, TCS34725, DS18B20, level-converter LV side |

All grounds are common.

## I²C bus — SDA = GPIO32, SCL = GPIO33
`Wire.begin(32, 33)`.

| Device | Part | Address | Supply | Bus connection |
|---|---|---|---|---|
| pH ADC | ADS1115 (16-bit) | 0x48 | **5 V** | **through the level converter (HV side)** |
| Colour | TCS34725 breakout | 0x29 | 3.3 V | direct to GPIO32 / GPIO33 |
| RTC | DS3231 | 0x68 | 3.3 V | direct to GPIO32 / GPIO33 |
| Display | LCD 16×2 (PCF8574 backpack) | 0x27 | **5 V** | direct to GPIO32 / GPIO33 |

### Level converter — LLC-4CH-I2C (bidirectional, 3.3 V ↔ 5 V)
| Side | Pin | Connection |
|---|---|---|
| LV (3.3 V) | LV / GND | 3.3 V bus / GND |
| LV | LV1 / LV2 | ESP32 GPIO33 (SCL) / GPIO32 (SDA) |
| HV (5 V) | HV / GND | 5 V bus / GND |
| HV | HV1 / HV2 | ADS1115 SCL / SDA |

> ⚠️ **If you build a copy:** the LCD backpack runs at 5 V and is wired straight to
> GPIO32/33, so its on-board pull-up resistors pull the I²C lines towards 5 V,
> above the ESP32's 3.3 V pin rating. The original unit works this way, but for a
> new build either connect the LCD to the HV side of the level converter alongside
> the ADS1115, or remove the pull-ups on the PCF8574 backpack.

## pH front-end
| From | To |
|---|---|
| E-201-C glass combination electrode (BNC) | analog pH interface board |
| pH interface board VCC / GND | 5 V / GND |
| pH interface board PO | ADS1115 AIN0 (single-ended) |
| ADS1115 VDD / GND | 5 V / GND |
| ADS1115 SCL / SDA | level converter HV1 / HV2 |

## EC meter — RS-485 / Modbus (`Serial2` 9600 8N1, slave id 1)
| ESP32 | MAX485 | Notes |
|---|---|---|
| GPIO2 | DE + RE (tied) | `MAX485_DE_RE`, strapping pin |
| GPIO16 (RX) | RO | `MAX485_RX` |
| GPIO17 (TX) | DI | `MAX485_TX` |
| 5V / GND | VCC / GND | |
| — | A / B → MI-Water-EC485 transmitter (yellow / blue) | reads holding register 0x0000 |

The EC transmitter itself is powered from the 12 V supply (red V+, black GND).

## Temperature — DS18B20 (1-Wire)
| Signal | Pin | Notes |
|---|---|---|
| DQ (data) | GPIO4 | `ONE_WIRE_BUS`, 4.7 kΩ pull-up to 3V3 |
| VDD / GND | 3V3 / GND | |

## Titrant mass — HX711 + load cell
| HX711 | ESP32 |
|---|---|
| DOUT | GPIO25 (`LOADCELL_DOUT_PIN`) |
| SCK | GPIO26 (`LOADCELL_SCK_PIN`) |
| VCC / GND | 5V / GND |
| E+/E-/A+/A- | load cell bridge wires |

## microSD — SPI (VSPI defaults)
| Signal | Pin |
|---|---|
| CS | GPIO5 (`SD_CS_PIN`) |
| SCK | GPIO18 |
| MISO | GPIO19 |
| MOSI | GPIO23 |
| VCC / GND | 5V / GND |

## Pump relay
| Signal | Connection |
|---|---|
| Relay IN | GPIO13 (`RELAY_PIN`) |
| VCC / GND | 5V / GND |
| COM | LM2596 #2 OUT+ |
| NO | pump red (+) |
| Pump black (−) | common ground |

*Single 1-channel 5 V relay module (active-high). The pump is a 12 V / 3 W DC
peristaltic pump run at about 7.5 V from LM2596 #2. Not a WeMos shield.*

## Buttons (to GND, internal pull-ups)
| Button | Pin | Notes |
|---|---|---|
| Button 1 | GPIO27 (`BUTTON1_PIN`) | start / stop titration |
| Button 2 | GPIO14 (`BUTTON2_PIN`) | start / stop rinse |
| Button 3 | GPIO15 (`BUTTON3_PIN`) | record-only logging; strapping pin |

---

## Sketch fix checklist (`msat-wiring.fzz`)
The current sketch needs the following to match the wiring above:

**Remove**
- [ ] Both *WeMos D1-Mini Relay Shield* parts (wrong form factor for ESP32).

**Add**
- [ ] 12 V supply and a second LM2596 (pump supply).
- [ ] LLC-4CH-I2C level converter between GPIO32/33 and the ADS1115; power the ADS1115 from 5 V.
- [ ] 1× 1-channel 5V relay module → GPIO13, COM from LM2596 #2.
- [ ] DS3231 RTC → I²C (0x68).
- [ ] LCD 16×2 I²C → I²C (0x27), 5 V.
- [ ] microSD card module → VSPI, CS GPIO5.
- [ ] 2 more push buttons (duplicate the existing one) → GPIO14, GPIO15.
- [ ] EC transmitter on the MAX485 A/B lines, powered from 12 V.
- [ ] pH electrode and interface board into ADS1115 AIN0.

**Already present & correct:** ESP32-38pin, ADS1115, GY-33, DS18B20, HX711,
MAX485, LM2596, 1× push button.

**After editing:** `File ▸ Save` in Fritzing so all parts re-embed into the
`.fzz` (this keeps the file portable on other machines).

## Fritzing parts to download
Already bundled in `fritzing-parts/`: ADS1115, DS18B20, GY-33, WAGO.
Still needed — download the `.fzpz`, then in Fritzing use **Part ▸ Import…**,
place + wire per the tables above, and **File ▸ Save** (re-embeds everything):

| Module | Source (`.fzpz`) |
|---|---|
| DS3231 RTC | [Soldered/e-radionica library](https://github.com/SolderedElectronics/e-radionica.com-Fritzing-Library-parts-/blob/master/DS3231%20RTC.fzpz) · [Adafruit](https://github.com/adafruit/Fritzing-Library/blob/master/parts/Adaruit%20DS3231.fzpz) · core part `rtc_ds3231_breakout` |
| LCD 16×2 I²C | [johnyHV LCD1602-I2C](https://github.com/johnyHV/fritzing-parts/blob/master/LCD1602-I2C.fzpz) · [Soldered "LCD screen 16x2 IIC"](https://github.com/SolderedElectronics/e-radionica.com-Fritzing-Library-parts-/blob/master/LCD%20screen%2016x2%20IIC.fzpz) |
| microSD module | [coderfls "Catalex MicroSD Module"](https://github.com/coderfls/Fritzing-Parts/blob/main/Catalex%20MicroSD%20Module.fzpz) · [robertoostenveld "SD Card Module"](https://github.com/robertoostenveld/fritzing/blob/master/SD%20Card%20Module.fzpz) |
| 1-channel 5V relay | [coderfls "5V Relay Module"](https://github.com/coderfls/Fritzing-Parts/blob/main/5V%20Relay%20Module.fzpz) · [KY-019](https://github.com/coderfls/Fritzing-Parts/blob/main/KY-019%205V%20Relay%20Module.fzpz) |
| MAX485 / RS-485 | [Warlib1975 "RS485 module MAX485"](https://github.com/Warlib1975/Fritzing-parts/blob/master/RS485%20module%20MAX485.fzpz) (SparkFun MAX485 also in Fritzing core) |
| Level converter | Fritzing core part "Logic Level Converter" (SparkFun BOB-12009), 4 channels |
| pH probe + board | label a generic 2-terminal BNC input on a generic analog board feeding ADS1115 AIN0 |
| EC transmitter | use a labelled generic sensor on the MAX485 A/B lines (no standard part) |

> On GitHub, open the file and click **Download raw file** to get the `.fzpz`.

## Quick reference figure
`msat-wiring-schematic.svg` is a high-level block diagram of the wiring above.

## Detailed harness diagram (WireViz)
`msat-wiring.yml` is the WireViz source of truth: every connector with every
pin, every cable with explicit pin-to-pin connections, colour-coded by bus.
Rendered outputs in this folder:

- `msat-wiring.html` — interactive viewer with embedded BOM (open in browser)
- `msat-wiring.svg` / `msat-wiring.png` — harness diagram
- `msat-wiring.bom.tsv` — connector and cable list

Re-render after editing the YAML:
```
pip install wireviz                            # one-time
winget install Graphviz.Graphviz               # one-time (system, needs `dot`)
wireviz msat-wiring.yml                         # produces .html/.svg/.png/.bom.tsv
```

`msat-wiring-components.xlsx` holds the full wiring list and the purchased bill
of materials (30 lines, THB and USD at 32.43 THB per USD), identical to
Supplementary Tables S1 and S2.

---
Hardware design © 2026 Burapha University · CERN-OHL-S v2 · Patent pending No. 2603001145
