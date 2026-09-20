// ble_scan.h
#pragma once
#include <Arduino.h>
#include <vector>

// Passive BLE advertisement discovery only - lists nearby devices the radio
// already receives. No pairing, connecting, or transmit features.

void bleInit();

// Scans for the given duration (seconds) and returns one summary line per
// device, strongest signal first: "DeviceName  aa:bb:cc:dd:ee:ff  -60dBm"
std::vector<String> bleScanDevices(uint32_t scanSeconds = 4);

// Anti-stalking tool: flags nearby Apple Find My advertisements (the
// public, documented beacon format AirTags and similar trackers use).
// Best-effort - matches a known manufacturer-data signature, not a
// guarantee of detecting every tracker model.
std::vector<String> bleScanAirTags(uint32_t scanSeconds = 4);

// Flags BLE devices whose advertised name matches signatures commonly
// reused in card-skimmer hardware (generic serial-Bluetooth modules).
// Heuristic only - a hit is a reason to look closer, not proof of a
// skimmer, and a clean scan doesn't guarantee there isn't one.
std::vector<String> bleScanSkimmers(uint32_t scanSeconds = 4);