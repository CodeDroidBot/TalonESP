# TalonESP

**A handheld wireless research platform built on the ESP32-S3.**

[![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue?logo=espressif)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Framework](https://img.shields.io/badge/framework-Arduino%20%7C%20PlatformIO-orange?logo=platformio)](https://platformio.org/)
[![Status](https://img.shields.io/badge/status-active-brightgreen)]()

---

TalonESP is an open-source, pocket-sized instrument for wireless research, RF analysis, and hardware exploration. It consolidates Sub-GHz, WiFi, Bluetooth LE, infrared, LoRa, GPS, and native USB HID into a single self-contained device with a color TFT display and a six-button interface.

The system runs entirely on-device. There is no cloud dependency, no telemetry, and no account requirement. All captures, scripts, and logs are stored on a removable microSD card using open, interoperable formats: Flipper-compatible `.sub` and `.ir` files, standard `.pcap` captures, DuckyScript payloads, and WiGLE CSV wardriving output. Every artifact TalonESP produces can be read, edited, or analyzed with existing tools.

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

Universal remote and signal tooling for consumer IR devices.

| Menu Item | Function |
|-----------|----------|
| **TV-B-Gone** | Cycle through power-off codes for common TV brands to switch off any nearby display. |
| **Custom IR** | Browse and replay saved `.ir` signal files from the SD card. |
| **IR Read** | Capture incoming IR signals and save them in Flipper `.ir` format. |
| **IR Jammer** | Emit continuous IR noise to interfere with IR-controlled devices. |
| **Universal Remote** | Icon-based remote control with SD-loaded skins for TVs, ACs, audio gear, and projectors. |

### WiFi (2.4 GHz)

Passive and active tools sharing a single radio.

| Menu Item | Function |
|-----------|----------|
| **Scan** | Enumerate nearby access points and list them sorted by RSSI. |
| **Signal strength** | Display live RSSI bars for all visible networks as signal changes over time. |
| **Packet Monitor** | Count management, control, and data frames with a 13-channel waterfall visualization. |
| **Deauth Detect** | Passively count deauth and disassoc frames to flag possible attacks. |
| **Deauth** | Inject targeted deauth frames at a chosen AP to disconnect its clients. |
| **Beacon Spam** | Flood the air with randomized beacon frames advertising fake SSIDs. |
| **Channel Analyzer** | Show per-channel activity histograms to identify congested or quiet bands. |
| **Sniffer** | Capture promiscuous 802.11 traffic and log packet metadata to SD in CSV. |
| **Connect (saved)** | Join a saved network from `/wifi_creds.csv` and display the assigned IP. |
| **Scan Hosts** | Probe the local subnet for live hosts on TCP port 80 (HTTP). |
| **Evil Portal** | Serve a configurable captive-portal page and log submitted credentials. |
| **Handshake Capture** | Capture a full WPA 4-way handshake with automatic deauth, saved as hashcat-ready `.pcap`. |

### Bluetooth LE

Passive scanning and threat detection.

| Menu Item | Function |
|-----------|----------|
| **Scan** | Enumerate nearby BLE advertisers with name, MAC, and RSSI, sorted by signal strength. |
| **AirTag Sniffer** | Flag Apple Find My beacon advertisements for anti-stalking detection. |
| **Skimmer Detect** | Flag BLE devices matching common card-skimmer naming patterns. |

### BadUSB

Full DuckyScript 1.0 interpreter with native USB HID execution.

| Menu Item | Function |
|-----------|----------|
| **Type (Keyboard)** | Type a fixed test string over USB HID to verify the target recognizes the device. |
| **Mouse Control** | Placeholder for USB mouse HID (not implemented in this build). |
| **Custom Script** | Browse `/badusb/*.txt` on SD and execute the selected DuckyScript payload. |

**Supported DuckyScript commands:** `STRING` · `STRINGLN` · `DELAY` · `DEFAULT_DELAY` · modifier combos · `REPEAT` · `VAR` · `DEFINE` · `IF` · `WHILE` · `FUNCTION` / `CALL`

**Keyboard layouts:** US · DE · FR · ES · IT · PT-BR · PT-PT · SV · DA · HU

### GPS

Positioning, logging, and location-tagged scanning.

| Menu Item | Function |
|-----------|----------|
| **Live Fix** | Display latitude, longitude, altitude, speed, compass heading, and satellite quality. |
| **Log Track** | Periodically append current position to a CSV track log on SD. |
| **Waypoints** | Save, view, delete, and navigate to saved waypoints with bearing-to-target display. |
| **Wardrive** | Log every unique BSSID seen with its GPS position in WiGLE-compatible CSV format. |

### LoRa (SX127x)

Point-to-point messaging and packet analysis.

| Menu Item | Function |
|-----------|----------|
| **Chat / Beacon** | Send and receive short text messages between TalonESP devices over LoRa. |
| **Packet Monitor** | Display live RSSI, SNR, and RX/TX counters for every received packet. |
| **Settings** | Adjust band (433/868/915 MHz), spreading factor, and TX power at runtime. |

### GPIO Workbench

Bus Pirate-style pin control and protocol tooling.

| Menu Item | Function |
|-----------|----------|
| **Pin Control (Header)** | Set any of the 10 header pins to input, input-pullup, output, or PWM. |
| **Pin Control (Expander)** | Control the 10 MCP23017 expander pins with the same input/output/PWM modes. |
| **I2C** | Scan the bus and perform read, write, write-read, bus-speed, and slave-mode operations. |
| **SPI** | Transfer bytes with configurable mode and clock frequency, plus SPI slave mode. |
| **UART** | Terminal and bridge with configurable baud, parity, and stop bits. |
| **1-Wire** | Detect presence and read the ROM ID of connected 1-Wire devices. |
| **2-Wire** | Reserved for ISO 7816 smart-card and I2C-sniffing work. |
| **3-Wire** | Reserved for 3-wire EEPROM and SPI-like device work. |
| **NFC** | PN532 reader support for ISO 14443A/B tag detection. |

### Interface

| Menu Item | Function |
|-----------|----------|
| **Theme** | Cycle between Terminal Green, Orange/Grey, and RGB Cycle animated palettes. |
| **Mode** | Toggle between Dark and Light variants of the current theme. |
| **Brightness** | Adjust the TFT backlight level from 16 to 255. |
| **Orientation** | Rotate the display between 0°, 90°, 180°, and 270°. |
| **Timeout** | Set the display idle dimming timer (Never, 30s, 1m, 5m, 10m, 30m). |
| **Debounce** | Adjust the button debounce window from 5 to 100 ms. |

### Games

| Menu Item | Function |
|-----------|----------|
| **Snake** | Classic grid snake with score and speed progression. |
| **Tetris** | Falling-block puzzle with hold, levels, and hard drop. |
| **Pong** | Breakout-style solo paddle game with three lives. |
| **Jumper** | Endless side-scrolling obstacle jumper with progressive difficulty. |
| **Duel** | Two-fighter versus-AI combat with move, punch, and block. |

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
