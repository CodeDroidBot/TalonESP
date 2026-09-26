#pragma once
#include <Arduino.h>

// ---------------- Device identity ----------------
// Shown on the About/boot screen. Change this to whatever you want to call
// your build - it's just a display string, nothing else depends on it.
#define DEVICE_NAME "GreyHat"
#define DEVICE_AUTHOR "by CodeDroidBot"
// ---------------- Shared SPI bus (TFT + SD + CC1101) ----------------
#define PIN_SPI_SCK   12
#define PIN_SPI_MOSI  11
#define PIN_SPI_MISO  13

#define PIN_TFT_CS    10
#define PIN_TFT_DC    9
#define PIN_TFT_RST   14
#define PIN_TFT_BL    15

#define PIN_SD_CS     21

// CC1101 SPI pins (use the same as TFT if shared)
#define PIN_CC1101_SCK   TFT_SCLK
#define PIN_CC1101_MISO  TFT_MISO
#define PIN_CC1101_MOSI  TFT_MOSI
#define PIN_CC1101_CS   8
#define PIN_CC1101_GDO0 2
#define PIN_CC1101_GDO2 -1

// ---------------- I2C bus (MCP23017) ----------------
#define PIN_I2C_SDA   17
#define PIN_I2C_SCL   18
#define PIN_MCP_INT   16
#define MCP23017_ADDR_OFFSET 0   // A0/A1/A2 all tied to GND -> 0x20

// ---------------- IR ----------------
#define PIN_IR_RX     6
#define PIN_IR_TX     7

// ---------------- Buttons (MCP23017 port A pins) ----------------
#define BTN_UP     0
#define BTN_DOWN   1
#define BTN_LEFT   2
#define BTN_RIGHT  3
#define BTN_OK     4
#define BTN_BACK   5

// ---------------- GPS (UART) - not yet wired up in code, pins reserved ----------------
#define PIN_GPS_RX    2   // <- GPS module TX
#define PIN_GPS_TX    1   // -> GPS module RX (optional)

// ---------------- LoRa (SPI, e.g. SX127x/RFM95) - pins reserved but currently
#define PIN_LORA_CS    38
#define PIN_LORA_RST   39
#define PIN_LORA_DIO0  40  // IRQ pin most LoRa libraries require
#define PIN_LORA_DIO1  41  // only needed by some libraries (e.g. RadioLib in LoRaWAN mode)

#define PIN_GPIO_IO1   3
#define PIN_GPIO_IO2   4
#define PIN_GPIO_IO3   5
#define PIN_GPIO_IO4   42
#define PIN_GPIO_IO5   47
#define PIN_GPIO_IO6   48
#define PIN_GPIO_IO7   PIN_LORA_CS
#define PIN_GPIO_IO8   PIN_LORA_RST
#define PIN_GPIO_IO9   PIN_LORA_DIO0
#define PIN_GPIO_IO10  PIN_LORA_DIO1

// ---------------- Status LED (onboard NeoPixel) ----------------
#define PIN_NEOPIXEL     48
#define NEOPIXEL_COUNT   1

// State IDs for the application – used by modules to request state changes.
// These must match the order of the AppState enum in main.cpp.
// ============================================================
//  State IDs – must match the AppState enum in main.cpp
// ============================================================
#define STATE_MAIN_MENU              0
#define STATE_SUBGHZ_MENU            1
#define STATE_SUBGHZ_LISTEN          2
#define STATE_SUBGHZ_SCAN            3
#define STATE_SUBGHZ_INFO            4
#define STATE_SUBGHZ_JAM             5
#define STATE_SUBGHZ_SAVED           6
#define STATE_SUBGHZ_SAVED_ACTION    7
#define STATE_INFRARED_MENU          8
#define STATE_INFRARED_CAPTURE       9
#define STATE_INFRARED_UNIVERSAL_CATEGORY 10
#define STATE_INFRARED_UNIVERSAL_BRAND    11
#define STATE_INFRARED_UNIVERSAL     12
#define STATE_INFRARED_PLAY_FILE     13
#define STATE_FILE_MANAGER           14
#define STATE_FILE_ACTION            15
#define STATE_WIFI_MENU              16
#define STATE_WIFI_SCAN              17
#define STATE_WIFI_METER             18
#define STATE_WIFI_DEAUTH            19
#define STATE_WIFI_DEAUTH_PICK       20
#define STATE_WIFI_DEAUTH_RUN        21
#define STATE_WIFI_BEACON_PICK       22
#define STATE_WIFI_BEACON_RUN        23
#define STATE_WIFI_PACKET_MON        24
#define STATE_BLE_MENU               25
#define STATE_BLE_SCAN               26
#define STATE_BLE_AIRTAG             27
#define STATE_BLE_SKIMMER            28
#define STATE_HID_MENU               29
#define STATE_HID_SELECT_MODE        30
#define STATE_HID_KEYBOARD           31
#define STATE_HID_MOUSE              32
#define STATE_HID_SCRIPT_SELECT      33
#define STATE_HID_SCRIPT_RUN         34
#define STATE_GAMES_MENU             35
#define STATE_GAME_SNAKE             36
#define STATE_GAME_TETRIS            37
#define STATE_GAME_PONG              38
#define STATE_SETTINGS               39
#define STATE_ABOUT                  40
#define STATE_GPS_MENU               41
#define STATE_GPS_LIVE               42
#define STATE_GPS_LOG                43
#define STATE_GPS_WAYPOINTS          44
#define STATE_GPS_WAYPOINT_VIEW      45
#define STATE_LORA_MENU              46
#define STATE_LORA_CHAT              47
#define STATE_LORA_MONITOR           48
#define STATE_LORA_SETTINGS          49
#define STATE_GPS_WARDRIVE           50
#define STATE_WIFI_CHANNEL_ANALYZER  51
#define STATE_WIFI_SNIFFER           52
#define STATE_WIFI_CONNECT           53
#define STATE_WIFI_SCANHOSTS         54
#define STATE_WIREGUARD              55
#define STATE_HID_SCRIPT_PICKER      56
#define STATE_IR_PLAY_PICKER         57
#define STATE_INFRARED_TVBGONE       58
#define STATE_INFRARED_JAMMER        59
#define STATE_GPIO_MENU              60
#define STATE_GPIO_DIO               61
#define STATE_GPIO_EXPANDER          62
#define STATE_GPIO_I2C_MENU          63
#define STATE_GPIO_I2C_SCAN          64
#define STATE_GPIO_SPI_MENU          65
#define STATE_GPIO_SPI_PROBE         66
#define STATE_GPIO_UART_MENU         67
#define STATE_GPIO_1WIRE             68
#define STATE_GPIO_2WIRE             69
#define STATE_GPIO_3WIRE             70
#define STATE_GPIO_NFC               71
#define STATE_GPIO_UART_ACTIVE       72
#define STATE_WIFI_EVIL_PORTAL       73
#define STATE_WEB_UI                 74

// ---- I2C sub-states ----
#define STATE_I2C_READ         75
#define STATE_I2C_WRITE        76
#define STATE_I2C_WRITEREAD    77
#define STATE_I2C_SPEED        78
#define STATE_I2C_SLAVE        79

// ---- SPI sub-states ----
#define STATE_SPI_TRANSFER     80
#define STATE_SPI_MODE         81
#define STATE_SPI_FREQ         82
#define STATE_SPI_SLAVE        83

// ---- UART sub-states ----
#define STATE_UART_TERMINAL    84
#define STATE_UART_SETTINGS    85

// ---- 3-Wire sub-states ----
#define STATE_THREEWIRE_READ   86
#define STATE_THREEWIRE_WRITE  87
#define STATE_THREEWIRE_ERASE  88

// ---- WiFi Handshake ----
#define STATE_WIFI_HANDSHAKE_MENU    89
#define STATE_WIFI_HANDSHAKE_CAPTURE 90
#define STATE_WIFI_HANDSHAKE_RESULT  91

// ---- BLE ----
#define STATE_BLE_SPAM               92

// ---- GPIO peripherals (nRF24, CAN, RFID) ----
// Note: STATE_GPIO_NFC is already defined above at 71 (existing value).
// STATE_GPIO_RFID is new, and gets a fresh number.
#define STATE_GPIO_NRF               93
#define STATE_GPIO_CAN               94
#define STATE_GPIO_RFID              95
