#pragma once
#include <Arduino.h>

enum class LoraBand { MHz433, MHz868, MHz915, Count };

// Brings up the SX127x/RFM95 over the shared SPI bus (same pattern as the
// CC1101 sub-GHz radio: its own CS/RST/IRQ, everything else shared).
// Returns true if the radio responded and initialized OK.
bool loraInit();
bool loraIsReady();
void loraDeinit();  
// ---- TX ----
bool loraSend(const String& text);

// ---- RX ----
// Call every loop() iteration. Returns true if a new packet arrived and
// fills outText/outRssi/outSnr; otherwise leaves them untouched.
bool loraPoll(String& outText, int& outRssi, float& outSnr);

uint32_t loraGetRxCount();
uint32_t loraGetTxCount();
int loraGetLastRssi();
float loraGetLastSnr();
unsigned long loraGetLastRxMs(); // millis() timestamp of last RX, 0 if none yet

// ---- Runtime-adjustable radio settings (mirrors the Settings screen style) ----
void loraSetBand(LoraBand band);
LoraBand loraGetBand();
const char* loraGetBandName();

// Spreading factor 6-12 (higher = longer range, slower/more airtime).
void loraSetSpreadingFactor(int sf);
int loraGetSpreadingFactor();

// TX power in dBm, roughly 2-20.
void loraSetTxPower(int dbm);
int loraGetTxPower();

void loraEnterMenu();
void loraHandleMenuEvent(int evt);

void loraEnterChat();
void loraHandleChatEvent(int evt, bool gotPacket, const String& packet, int rssi, float snr);

void loraEnterMonitor();
void loraHandleMonitorEvent(int evt, bool gotPacket);

void loraEnterSettings();
void loraHandleSettingsEvent(int evt);