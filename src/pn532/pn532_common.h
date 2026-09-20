#pragma once
#include <Arduino.h>
#include <PN532.h>
#include <PN532_I2C.h>

// ============================================================
//  Shared PN532 driver — one instance used by both the NFC
//  and RFID menus.
//
//  Wiring (I2C mode, default):
//    IO1 = SDA, IO2 = SCL
//
//  To use SPI instead, edit pn532_common.cpp — swap the include
//  and the pn532_iface instance for the SPI variant.
// ============================================================

// Underlying interface object (I2C by default). The EmulateTag
// class needs a reference to this directly.
extern PN532_I2C pn532_iface;

// High-level driver used by both menus.
extern PN532 nfc;

bool pn532Begin();           // idempotent — safe to call every entry
bool pn532IsReady();
uint32_t pn532GetFirmwareVersion();
void pn532End();

// Reads one ISO14443A target UID. Blocks up to timeoutMs.
bool pn532ReadUid(uint8_t* uid, uint8_t& uidLen, uint16_t timeoutMs = 1000);

// "08:11:22:33" style formatting.
String pn532UidToString(const uint8_t* uid, uint8_t len);