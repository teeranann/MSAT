# MSAT Firmware (ESP32)

Arduino firmware for the MSAT device. Reads pH / EC / colour / temperature /
load cell, runs the titration state machine (IDLE → PENDING → RUNNING →
RINSING / REC), serves a WiFi dashboard + WebSocket telemetry, and logs every
run to the SD card.

## Board & toolchain
- **Board:** ESP32 (ESP32‑WROOM, e.g. ESP32‑D0WD‑V3)
- **Arduino‑ESP32 core** 3.x, Arduino IDE or arduino‑cli

## Required libraries (Library Manager)
- ESPAsyncWebServer + AsyncTCP (ESP32Async fork for core 3.x)
- Adafruit ADS1X15 (pH ADC)
- Adafruit TCS34725 (colour) *(GY‑33/TCS34725)*
- DallasTemperature + OneWire (DS18B20)
- HX711 (load cell)
- ModbusMaster (RS‑485 EC meter)
- RTClib (DS3231)
- LiquidCrystal_I2C (16×2 LCD)

## Pin map
| Function | Pin | Notes |
|---|---|---|
| I²C SDA / SCL | 32 / 33 | ADS1115 (pH 0x48), TCS34725 (0x29), RTC (0x68), LCD (0x27) |
| RS‑485 DE/RE, RX, TX | 2, 16, 17 | EC meter (Modbus) — **GPIO2 is a strapping pin** |
| Load cell DOUT / SCK | 25 / 26 | HX711 |
| DS18B20 (1‑Wire) | 4 | temperature |
| SD card CS | 5 | SPI (VSPI) |
| Relay (pump) | 13 | titrant delivery |
| Buttons 1 / 2 / 3 | 27 / 14 / 15 | **GPIO15 is a strapping pin** |

> ⚠️ **Flashing:** GPIO2 and GPIO15 are ESP32 strapping pins. If wires are
> attached there during upload, flashing can fail — **disconnect them while
> flashing**, then reconnect.

## Configure before flashing
Edit the lines marked `EDIT ME` near the top of
`msat-firmware/msat-firmware.ino`:
```cpp
const char* ssid     = "YOUR_WIFI_SSID";       // 2.4 GHz network
const char* password = "YOUR_WIFI_PASSWORD";
#define ADMIN_PASSWORD "CHANGE_ME"
```
The device uses a **static IP `192.168.1.200`** (gateway `192.168.1.1`) — change
`local_IP`/`gateway` if your network differs. After boot, open
`http://192.168.1.200/` for the dashboard.

## Sensor calibration
pH (2‑point), EC (KCl standards), and load cell are calibrated via the
device/analyzer; see [`../hardware/`](../hardware/).

---
Copyright © 2026 Burapha University · Apache-2.0 · Patent pending No. 2603001145
