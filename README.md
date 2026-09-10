# TalonESP

**A handheld wireless research platform built on the ESP32-S3.**

[![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue?logo=espressif)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Framework](https://img.shields.io/badge/framework-Arduino%20%7C%20PlatformIO-orange?logo=platformio)](https://platformio.org/)
[![Status](https://img.shields.io/badge/status-active-brightgreen)]()

---

TalonESP is an open-source, pocket-sized instrument for wireless research, RF analysis, and hardware exploration. It consolidates Sub-GHz, WiFi, Bluetooth LE, infrared, LoRa, GPS, and native USB HID into a single self-contained device with a color TFT display and a six-button interface.

The system runs entirely on-device. There is no cloud dependency, no telemetry, and no account requirement. All captures, scripts, and logs are stored on a removable microSD card using open, interoperable formats: Flipper-compatible `.sub` and `.ir` files, standard `.pcap` captures, DuckyScript payloads, and WiGLE CSV wardriving output. Every artifact TalonESP produces can be read, edited, or analyzed with existing tools.

> *Sharp. Precise. Catches everything.*

---

## Table of Contents

- [Capabilities](#capabilities)
- [Hardware](#hardware)
- [Getting Started](#getting-started)
- [SD Card Layout](#sd-card-layout)
- [Usage](#usage)
- [Project Structure](#project-structure)
- [Responsible Use](#responsible-use)
- [Credits](#credits)
- [Contributing](#contributing)
- [License](#license)

---

## Capabilities

### Sub-GHz (CC1101)

Coverage across 315, 433.92, 868, and 915 MHz.

| Mode | Description |
|------|-------------|
| **Read** | Capture, decode, and replay OOK/ASK remotes with automatic frequency hopping |
| **Read RAW** | Record raw pulse trains for later replay |
| **Send** | Browse and transmit saved `.sub` files (Flipper-compatible) |
| **Analyzer** | Live RSSI scan with auto-calibrated noise floor and per-frequency history |
| **Jammer** | Continuous carrier transmission on the selected frequency |
| **Brute** | Fixed-code bruteforcer: CAME, NICE, Holtek, Chamberlain, Ansonic, Linear Delta-3, Gate TX, SMC5326, MegaCode, UNILARM |

### Infrared

- **Universal Remote** — icon-based skins loaded from SD (TV, AC, audio, projector)
- **IR Read** — capture and save signals in Flipper `.ir` format
- **Custom IR** — replay from the saved signal library
- **TV-B-Gone** — power-off sequence for common TV brands
- **IR Jammer** — continuous IR noise emission

### WiFi (2.4 GHz)

Passive and active tools sharing a single radio.

**Passive**
- Network scan (RSSI-sorted)
- Signal meter with live bars
- Packet monitor with 13-channel waterfall
- Channel activity analyzer
- Promiscuous sniffer (CSV output to SD)
- Passive deauth/disassoc detector

**Active**
- Targeted deauth injection
- Beacon spam
- Evil Portal with SD-loaded HTML pages and credential logging
- WPA handshake capture with automatic deauth (`.pcap` output, hashcat-ready)

**Utilities**
- Local subnet host discovery (TCP port 80)
- Saved network connection via `/wifi_creds.csv`

### Bluetooth LE

- Passive advertisement scanner, RSSI-sorted
- Apple Find My beacon detection (anti-stalking)
- Heuristic card-skimmer signature detection

### BadUSB

Full DuckyScript 1.0 interpreter with runtime-switchable keyboard layouts.

**Supported commands:** `STRING` · `STRINGLN` · `DELAY` · `DEFAULT_DELAY` · modifier combos · `REPEAT` · `VAR` · `DEFINE` · `IF` · `WHILE` · `FUNCTION` / `CALL`

**Keyboard layouts:** US · DE · FR · ES · IT · PT-BR · PT-PT · SV · DA · HU

Payloads load from `/badusb/*.txt` on SD. Execution uses the ESP32-S3's native USB peripheral — no external microcontroller required.

### GPS & Wardriving

- **Live Fix** — latitude, longitude, altitude, speed, compass, satellite quality
- **Log Track** — periodic CSV track logging
- **Waypoints** — save, view, delete, bearing-to-target
- **Wardriving** — WiGLE-format CSV of every unique BSSID, tagged with position

### LoRa (SX127x)

- **Chat / Beacon** — text messaging between devices
- **Packet Monitor** — live RSSI/SNR, RX/TX counters
- **Settings** — runtime band, spreading factor, TX power

### GPIO Workbench

| Mode | Features |
|------|----------|
| **Pin Control** | 10 header pins + 10 MCP23017 expander pins; input, input-pullup, output, PWM |
| **I2C** | Scanner, read, write, write-read, bus speed, slave mode |
| **SPI** | Transfer, mode, frequency, slave mode |
| **UART** | Terminal, bridge, configurable baud/parity/stop bits |
| **1-Wire** | Presence detect, ROM read |
| **2-Wire** | Reserved for smart-card work |
| **3-Wire** | Reserved for EEPROM work |
| **NFC** | PN532 reader support |

### Interface

- Custom icon-grid home screen
- 3 theme styles × 2 modes:
  - Terminal Green · Orange/Grey · **RGB Cycle** (animated hue rotation)
  - Dark · Light
- Adjustable brightness, idle timeout, display orientation

### Games

Snake · Tetris · Pong · Jumper · Duel

---

## Hardware

| Component | Part | Notes |
|-----------|------|-------|
| **MCU** | ESP32-S3 | 16 MB flash, 8 MB PSRAM (N16R8) |
| **Display** | ILI9341 | 240×320 SPI TFT |
| **Input** | MCP23017 | I2C GPIO expander, 6 buttons |
| **Sub-GHz** | CC1101 | Shared SPI bus |
| **Infrared** | RX + TX | 38 kHz demodulator and IR LED |
| **GPS** | NMEA UART module | 9600 baud default |
| **LoRa** | SX127x / RFM95 | SPI, pins reserved in `config.h` |
| **Storage** | microSD | Shared SPI bus |
| **Status LED** | WS2812 NeoPixel | Onboard, single pixel |

Pin assignments are defined in `include/config.h`. The SPI bus is shared between the display, SD card, CC1101, and LoRa module, each with its own chip-select line.

---

## Getting Started

### Requirements

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- ESP32-S3 dev board with 16 MB flash and 8 MB PSRAM
- Hardware wired per `include/config.h`
- FAT32-formatted microSD card

### Build and Flash

```bash
git clone https://github.com/YOUR-USER/talonesp.git
cd talonesp
pio run                    # compile
pio run -t upload          # flash
pio device monitor         # open serial monitor at 115200 baud
