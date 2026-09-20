#pragma once
#include <Arduino.h>

// RFID reader menu — low-level Mifare card operations.
// Uses the shared PN532 driver from pn532_common.h.
void gpioEnterRFID();
void gpioHandleRFIDEvent(int evt);