// shared.cpp
#include "shared.h"
#include "config.h"
#include "wifi_module.h"
#include "ir_module.h"
#include "gpio_module.h"
#include "badusb/hid_module.h"
#include "rf_module.h"
#include "lora_module.h"
#include "gps_module.h"
#include "uart/uart_module.h"

// ---- New modules ----
#include "nrf/nrf_module.h"
#include "canbus/can_module.h"
#include "pn532/pn532_common.h"

void powerInit() {
    // Nothing to do – all modules are already init'ed in setup()
}

void powerStopModule(PowerModule module) {
    switch (module) {
        case POWER_MODULE_WIFI_PACKET_MON:
            wifiStopPacketMonitor();
            break;
        case POWER_MODULE_WIFI_DEAUTH:
            wifiDeauthStop();
            break;
        case POWER_MODULE_WIFI_BEACON:
            wifiBeaconStop();
            break;
        case POWER_MODULE_IR_TVBGONE:
            irTvBGoneStop();
            break;
        case POWER_MODULE_IR_JAMMER:
            irJammerStop();
            break;
        case POWER_MODULE_UART_BRIDGE:
            gpioUartBridgeStop();
            break;
        case POWER_MODULE_HID_SCRIPT:
            hidStopScript();
            hidSetMode(HID_MODE_OFF);
            break;
        case POWER_MODULE_HID_KEYBOARD:
        case POWER_MODULE_HID_MOUSE:
            hidSetMode(HID_MODE_OFF);
            break;
        case POWER_MODULE_SUBGHZ:
            rfDeinit();
            break;
        case POWER_MODULE_LORA:
            loraDeinit();
            break;
        case POWER_MODULE_GPIO_SPI:
            // The dedicated SPI bus is stopped implicitly by gpioModuleSleep()
            break;
        default:
            break;
    }
}

void powerStopAll() {
    for (int m = POWER_MODULE_WIFI_PACKET_MON; m <= POWER_MODULE_LORA; m++) {
        powerStopModule((PowerModule)m);
    }
    gpioModuleSleep(); // also reset GPIO pins and buses
}

void powerExitState(int stateId) {
    switch (stateId) {
        // ---- WiFi states ----
        case STATE_WIFI_PACKET_MON:
        case STATE_WIFI_DEAUTH:
            powerStopModule(POWER_MODULE_WIFI_PACKET_MON);
            break;
        case STATE_WIFI_DEAUTH_RUN:
            powerStopModule(POWER_MODULE_WIFI_DEAUTH);
            break;
        case STATE_WIFI_BEACON_RUN:
            powerStopModule(POWER_MODULE_WIFI_BEACON);
            break;
        case STATE_WIFI_EVIL_PORTAL:
            wifiEvilPortalStop();
            break;

        // ---- IR states ----
        case STATE_INFRARED_TVBGONE:
            powerStopModule(POWER_MODULE_IR_TVBGONE);
            break;
        case STATE_INFRARED_JAMMER:
            powerStopModule(POWER_MODULE_IR_JAMMER);
            break;

        // ---- HID states ----
        case STATE_HID_SCRIPT_RUN:
            powerStopModule(POWER_MODULE_HID_SCRIPT);
            break;
        case STATE_HID_KEYBOARD:
        case STATE_HID_MOUSE:
            powerStopModule(POWER_MODULE_HID_KEYBOARD);
            break;

        // ---- Sub-GHz states ----
        case STATE_SUBGHZ_LISTEN:
        case STATE_SUBGHZ_SCAN:
        case STATE_SUBGHZ_INFO:
        case STATE_SUBGHZ_JAM:
        case STATE_SUBGHZ_SAVED:
        case STATE_SUBGHZ_SAVED_ACTION:
            powerStopModule(POWER_MODULE_SUBGHZ);
            break;

        // ---- LoRa states ----
        case STATE_LORA_CHAT:
        case STATE_LORA_MONITOR:
            powerStopModule(POWER_MODULE_LORA);
            break;

        // ---- GPS states ----
        case STATE_GPS_LIVE:
        case STATE_GPS_LOG:
        case STATE_GPS_WAYPOINTS:
        case STATE_GPS_WAYPOINT_VIEW:
        case STATE_GPS_WARDRIVE:
            gpsSetEnabled(false);
            break;

        // ---- GPIO hardware states (all active modes) ----
        // These all tear down the shared GPIO buses and float the
        // header + expander pins. NFC/RFID/nRF24/CAN have their own
        // dedicated cases below and are intentionally NOT in this list.
        case STATE_GPIO_DIO:
        case STATE_GPIO_EXPANDER:
        case STATE_GPIO_I2C_SCAN:
        case STATE_GPIO_SPI_PROBE:
        case STATE_GPIO_UART_ACTIVE:
        case STATE_GPIO_1WIRE:
        case STATE_GPIO_2WIRE:
        case STATE_I2C_READ:
        case STATE_I2C_WRITE:
        case STATE_I2C_WRITEREAD:
        case STATE_I2C_SPEED:
        case STATE_I2C_SLAVE:
        case STATE_SPI_TRANSFER:
        case STATE_SPI_MODE:
        case STATE_SPI_FREQ:
        case STATE_SPI_SLAVE:
        case STATE_THREEWIRE_READ:
        case STATE_THREEWIRE_WRITE:
        case STATE_THREEWIRE_ERASE:
            gpioModuleSleep();   // full teardown: buses, UART, pins floated
            break;

        // ---- UART terminal / bridge ----
        case STATE_UART_TERMINAL:
            gpioUartBridgeStop();
            break;

        // ---- nRF24 2.4 GHz radio ----
        case STATE_GPIO_NRF:
            nrfDeinit();
            break;

        // ---- CAN bus (TWAI or MCP2515) ----
        case STATE_GPIO_CAN:
            canDeinit();
            break;

        // ---- PN532 NFC / RFID ----
        // Both menus share the same PN532 chip and driver, so both
        // tear the same driver down.
        case STATE_GPIO_NFC:
        case STATE_GPIO_RFID:
            pn532End();
            break;

        // ---- GPIO menu states (no hardware active) ----
        case STATE_GPIO_MENU:
        case STATE_GPIO_I2C_MENU:
        case STATE_GPIO_SPI_MENU:
        case STATE_GPIO_UART_MENU:
        case STATE_GPIO_3WIRE:
        default:
            // Nothing to stop
            break;
    }
}