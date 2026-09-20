#pragma once
#include <Arduino.h>

enum BleSpamType {
    BLE_SPAM_IOS     = 0,
    BLE_SPAM_WINDOWS = 1,
    BLE_SPAM_SAMSUNG = 2,
    BLE_SPAM_ANDROID = 3,
    BLE_SPAM_ALL     = 4,
    BLE_SPAM_COUNT   = 5
};

const char* bleSpamName(BleSpamType type);

void bleSpamInit();
void bleSpamStart(BleSpamType type);
void bleSpamStop();
bool bleSpamIsRunning();
BleSpamType bleSpamCurrentType();
void bleSpamTick();
uint32_t bleSpamPacketsSent();

void bleSpamEnterMenu();
void bleSpamHandleEvent(int evt);

// Optional: call this early in setup() to disable the brownout detector
// while running BLE spam. BLE advertising causes brief current spikes
// that can trip the BOD on marginal power supplies (USB from a weak hub,
// small LiPo with thin wires, etc). Do NOT disable if you're actually
// running the chip out of spec - it's a safety net, not a nuisance.
void bleSpamDisableBrownout();