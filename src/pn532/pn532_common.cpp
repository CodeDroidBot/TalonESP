#include "pn532_common.h"
#include "config.h"
#include "gpio_module.h"
#include <Wire.h>

// ============================================================
//  Interface selection — pick ONE block below.
//  Default is I2C.
// ============================================================

// ---- I2C (default) ----
PN532_I2C pn532_iface(gpioGetWire());

// ---- SPI (uncomment and comment the I2C line above) ----
// #include <PN532_SPI.h>
// PN532_SPI pn532_iface(gpioGetSPI(), PIN_GPIO_IO4);

// ---- HSU (uncomment and comment the I2C line above) ----
// #include <PN532_HSU.h>
// PN532_HSU pn532_iface(Serial1);

// High-level driver that talks to the chip through the interface above.
PN532 nfc(pn532_iface);

// ============================================================
//  State
// ============================================================
static bool     pn532Ready   = false;
static bool     pn532Present = false;
static uint32_t pn532FwVer   = 0;

// ============================================================
//  Lifecycle
// ============================================================
bool pn532Begin() {
    if (pn532Ready) return pn532Present;

    // Bring up the GPIO I2C bus on IO1/IO2 before talking to the chip.
    gpioGetWire().begin(PIN_GPIO_IO1, PIN_GPIO_IO2, 100000);

    nfc.begin();
    pn532FwVer = nfc.getFirmwareVersion();

    if (!pn532FwVer) {
        Serial.println("[PN532] chip not responding");
        pn532Present = false;
        pn532Ready   = true;
        return false;
    }

    Serial.printf("[PN532] PN5%02X firmware %d.%d support 0x%02X\n",
                  (pn532FwVer >> 24) & 0xFF,
                  (pn532FwVer >> 16) & 0xFF,
                  (pn532FwVer >>  8) & 0xFF,
                   pn532FwVer        & 0xFF);

    // Required before passive target reads.
    nfc.SAMConfig();

    pn532Present = true;
    pn532Ready   = true;
    return true;
}

bool pn532IsReady() { return pn532Present; }

uint32_t pn532GetFirmwareVersion() { return pn532FwVer; }

void pn532End() {
    pn532Ready   = false;
    pn532Present = false;
    Serial.println("[PN532] deinit");
}

// ============================================================
//  Helpers
// ============================================================
bool pn532ReadUid(uint8_t* uid, uint8_t& uidLen, uint16_t timeoutMs) {
    if (!pn532Present) return false;
    uidLen = 0;
    return nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A,
                                    uid, &uidLen, timeoutMs);
}

String pn532UidToString(const uint8_t* uid, uint8_t len) {
    String s;
    for (uint8_t i = 0; i < len; i++) {
        if (i > 0) s += ":";
        if (uid[i] < 0x10) s += "0";
        s += String(uid[i], HEX);
    }
    s.toUpperCase();
    return s;
}