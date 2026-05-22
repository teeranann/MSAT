# MSAT Wiring (firmware-exact)

Connection spec for `msat-wiring.fzz`, derived directly from
[`../../firmware/msat-firmware/msat-firmware.ino`](../../firmware/msat-firmware/msat-firmware.ino).
Use this table to build or verify the Fritzing sketch. Pin numbers are ESP32
GPIO numbers.

> ⚠️ **Strapping pins:** GPIO2 (MAX485 DE/RE) and GPIO15 (Button 3) are ESP32
> strapping pins — disconnect them while flashing, then reconnect.

## I²C bus — SDA = GPIO32, SCL = GPIO33
`Wire.begin(32, 33)` — all four devices share this bus (plus 3V3 + GND).

| Device | Part | Address |
|---|---|---|
| pH ADC | ADS1115 (16-bit) | 0x48 |
| Colour | GY-33 / TCS34725 | 0x29 |
| RTC | DS3231 | 0x68 |
| Display | LCD 16×2 (PCF8574 backpack) | 0x27 |

## pH front-end
| From | To |
|---|---|
| pH BNC electrode | ADS1115 analog input (AINx) |
| ADS1115 VDD / GND | 3V3 / GND |
| ADS1115 SDA / SCL | GPIO32 / GPIO33 |

## EC meter — RS-485 / Modbus (`Serial2` 9600 8N1, slave id 1)
| ESP32 | MAX485 | Notes |
|---|---|---|
| GPIO2 | DE + RE (tied) | `MAX485_DE_RE`, strapping pin |
| GPIO16 (RX) | RO | `MAX485_RX` |
| GPIO17 (TX) | DI | `MAX485_TX` |
| 5V / GND | VCC / GND | |
| — | A / B → EC meter A / B | reads holding register 0x0000 |

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
| Signal | Pin |
|---|---|
| Relay IN | GPIO13 (`RELAY_PIN`) |
| VCC / GND | 5V / GND |
| COM / NO | pump power loop |

*Single 1-channel 5V relay module (active-high). Not a WeMos shield.*

## Buttons (to GND, internal pull-ups)
| Button | Pin | Notes |
|---|---|---|
| Button 1 | GPIO27 (`BUTTON1_PIN`) | |
| Button 2 | GPIO14 (`BUTTON2_PIN`) | |
| Button 3 | GPIO15 (`BUTTON3_PIN`) | strapping pin |

## Power
LM2596 buck converter steps the input supply down to 5V for the modules; the
ESP32 regulator provides 3V3 for the I²C/1-Wire sensors.

---

## Sketch fix checklist (`msat-wiring.fzz`)
The current sketch needs the following to match the firmware above:

**Remove**
- [ ] Both *WeMos D1-Mini Relay Shield* parts (wrong form factor for ESP32).

**Add**
- [ ] 1× 1-channel 5V relay module → GPIO13.
- [ ] DS3231 RTC → I²C (0x68).
- [ ] LCD 16×2 I²C → I²C (0x27).
- [ ] microSD card module → VSPI, CS GPIO5.
- [ ] 2 more push buttons (duplicate the existing one) → GPIO14, GPIO15.
- [ ] EC meter / Modbus RS-485 transmitter on the MAX485 A/B lines.
- [ ] pH BNC electrode into the ADS1115 input.

**Already present & correct:** ESP32-38pin, ADS1115, GY-33, DS18B20, HX711,
MAX485, LM2596, 1× push button.

**After editing:** `File ▸ Save` in Fritzing so all parts re-embed into the
`.fzz` (this keeps the file portable on other machines).

## Fritzing parts to download
Already bundled in `fritzing-parts/`: ADS1115, DS18B20, GY-33, WAGO.
Still needed — download the `.fzpz`, then in Fritzing use **Part ▸ Import…**,
place + wire per the table above, and **File ▸ Save** (re-embeds everything):

| Module | Source (`.fzpz`) |
|---|---|
| DS3231 RTC | [Soldered/e-radionica library](https://github.com/SolderedElectronics/e-radionica.com-Fritzing-Library-parts-/blob/master/DS3231%20RTC.fzpz) · [Adafruit](https://github.com/adafruit/Fritzing-Library/blob/master/parts/Adaruit%20DS3231.fzpz) · core part `rtc_ds3231_breakout` |
| LCD 16×2 I²C | [johnyHV LCD1602-I2C](https://github.com/johnyHV/fritzing-parts/blob/master/LCD1602-I2C.fzpz) · [Soldered "LCD screen 16x2 IIC"](https://github.com/SolderedElectronics/e-radionica.com-Fritzing-Library-parts-/blob/master/LCD%20screen%2016x2%20IIC.fzpz) |
| microSD module | [coderfls "Catalex MicroSD Module"](https://github.com/coderfls/Fritzing-Parts/blob/main/Catalex%20MicroSD%20Module.fzpz) · [robertoostenveld "SD Card Module"](https://github.com/robertoostenveld/fritzing/blob/master/SD%20Card%20Module.fzpz) |
| 1-channel 5V relay | [coderfls "5V Relay Module"](https://github.com/coderfls/Fritzing-Parts/blob/main/5V%20Relay%20Module.fzpz) · [KY-019](https://github.com/coderfls/Fritzing-Parts/blob/main/KY-019%205V%20Relay%20Module.fzpz) |
| MAX485 / RS-485 | [Warlib1975 "RS485 module MAX485"](https://github.com/Warlib1975/Fritzing-parts/blob/master/RS485%20module%20MAX485.fzpz) (SparkFun MAX485 also in Fritzing core) |
| pH BNC probe | [DFRobot Fritzing library](https://github.com/DFRobot/Fritzing-library) (SEN0161 pH kit); or label a generic 2-terminal BNC into ADS1115 AIN0 |
| EC meter | use a labelled generic 2-terminal sensor on the MAX485 A/B lines (no standard part) |

> On GitHub, open the file and click **Download raw file** to get the `.fzpz`.

## Quick reference figure
`msat-wiring-schematic.svg` (this folder) is a vector wiring diagram of the
full pin map above — open in any browser or Inkscape; export to PNG/PDF for
publication.

---
Hardware design © 2026 Burapha University · CC BY-NC 4.0 · Patent pending No. 2603001145
