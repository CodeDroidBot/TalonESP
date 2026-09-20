#pragma once
#include <Arduino.h>

// =============================================================================
// Deauth — 802.11 deauthentication frame injection + passive detection
// =============================================================================
//
// ---- Attack usage ----
//   Deauth::setTarget(ssid, bssidStr, channel);
//   Deauth::start();
//   // each loop():
//   Deauth::tick();
//   // ...
//   Deauth::stop();
//
// ---- Passive deauth detection (promiscuous mode, ESP32 / ESP32-S3) ----
//   Deauth::startDetect();          // hop all channels automatically
//   Deauth::startDetect(6);         // or pin to a specific channel
//   // each loop():
//   Deauth::tick();
//   uint32_t n = Deauth::detectedCount();
//   // ...
//   Deauth::stopDetect();
//
// Attack and detection can run concurrently (transmit + receive are independent
// in the ESP32/S3 WiFi driver when both use WIFI_IF_STA).
//
// The radio must already be in STA mode. Call wifiInit() (or WiFi.mode(WIFI_STA)
// + WiFi.disconnect()) before using any function here.
//
// ---- How promiscuous mode works on ESP32-S3 ----
// ESP32-S3 uses the same esp_wifi driver and the same API as the original
// ESP32 — there is no S3-specific change needed.
//
//   1. WiFi.mode(WIFI_STA);                      // must be in STA mode
//   2. esp_wifi_set_promiscuous(true);
//   3. esp_wifi_set_promiscuous_rx_cb(&myCallback);
//
//   Optional — filter which frame types reach the callback (recommended:
//   filtering in the driver is far cheaper than filtering in the callback):
//     wifi_promiscuous_filter_t f;
//     f.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;   // management only
//     // other masks: WIFI_PROMIS_FILTER_MASK_DATA, _CTRL, _ALL
//     esp_wifi_set_promiscuous_filter(&f);
//
//   4. esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);   // tune to channel
//   5. esp_wifi_set_promiscuous(false);                   // to stop
//
//   Callback requirements (true for ESP32 AND S3):
//     void IRAM_ATTR myCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
//       const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
//       // pkt->payload  — raw 802.11 MAC frame
//       // pkt->rx_ctrl.rssi, .channel, .sig_len
//       // MUST be fast: no Serial, no malloc, no blocking calls.
//       // MUST be IRAM_ATTR: code must be in IRAM, not flash.
//     }
//
// =============================================================================

namespace Deauth {

  // ---- Attack ----
  // setTarget() must be called before start(). Safe to call while stopped.
  void    setTarget(const String& ssid, const String& bssidStr, uint8_t channel);
  void    start();
  void    stop();
  bool    isRunning();
  uint32_t framesSent();
  String  getTargetSsid();
  String  getTargetBssid();
  uint8_t getChannel();

  // ---- Passive detection (promiscuous Rx) ----
  // channel = 0  → hop all channels 1-13 automatically (default)
  // channel 1-13 → stay fixed on that channel
  void    startDetect(uint8_t channel = 0);
  void    stopDetect();
  bool    isDetecting();
  uint32_t detectedCount();     // total deauth + disassoc frames seen since startDetect()
  void    resetDetectedCount();
  uint8_t detectChannel();      // channel currently being monitored

  // ---- Call every loop() ----
  // Handles frame injection (when running) and channel-hopping (when detecting).
  void tick();

} // namespace Deauth