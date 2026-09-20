#include "gps_module.h"
#include "config.h"
#include "sdcard.h"
#include "display.h"
#include "buttons.h"
#include "menu.h"
#include "helpers.h"
#include <TinyGPSPlus.h>
#include <WiFi.h>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <math.h>

// ====================================================================
//  Core GPS (unchanged)
// ====================================================================
static TinyGPSPlus gps;
static HardwareSerial gpsSerial(1);
static bool gpsEnabled = true;
static bool gpsInitialized = false; 
static bool logging = false;
static unsigned long lastLogMs = 0;
static uint32_t logPointCount = 0;
static const unsigned long LOG_INTERVAL_MS = 2000;
static const char* TRACK_PATH = "/gps_track.csv";
static const char* WAYPOINTS_PATH = "/gps_waypoints.csv";

void gpsInit() {
    // Lazy: only open the UART when a GPS screen is entered.
    if (gpsInitialized) return;
    gpsSerial.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    gpsInitialized = true;
}

bool gpsDetect(unsigned long timeoutMs) {
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        while (gpsSerial.available()) {
            char c = gpsSerial.read();
            if (gps.encode(c)) {
                // If we get a valid location or time, assume GPS is present
                if (gps.location.isValid() || gps.time.isValid()) {
                    return true;
                }
            }
        }
        delay(10);
    }
    return false;
} 

void gpsSetEnabled(bool enable) {
    if (!enable) {
        if (gpsInitialized) {
            gpsSerial.end();
            gpsInitialized = false;
        }
        gpsEnabled = false;
    } else {
        gpsInit();
        gpsEnabled = true;
    }
}

void gpsUpdate() {
    if (!gpsEnabled || !gpsInitialized) return;
    while (gpsSerial.available()) {
        gps.encode(gpsSerial.read());
    }
}

bool gpsHasFix() {
  return gps.location.isValid() && gps.location.age() < 5000;
}

GpsFix gpsGetFix() {
  GpsFix f{};
  f.valid = gpsHasFix();
  f.lat = gps.location.isValid() ? gps.location.lat() : 0.0;
  f.lon = gps.location.isValid() ? gps.location.lng() : 0.0;
  f.altitudeM = gps.altitude.isValid() ? gps.altitude.meters() : 0.0;
  f.speedKmh = gps.speed.isValid() ? gps.speed.kmph() : 0.0;
  f.courseDeg = gps.course.isValid() ? gps.course.deg() : 0.0;
  f.satellites = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
  f.hdop = gps.hdop.isValid() ? gps.hdop.hdop() : 99.9;
  f.hour = f.minute = f.second = 0;
  f.day = f.month = 0;
  f.year = 0;
  if (gps.time.isValid()) {
    f.hour = gps.time.hour();
    f.minute = gps.time.minute();
    f.second = gps.time.second();
  }
  if (gps.date.isValid()) {
    f.day = gps.date.day();
    f.month = gps.date.month();
    f.year = gps.date.year();
  }
  return f;
}

bool gpsBearingTo(double lat, double lon, double& outDistanceM, double& outBearingDeg) {
  if (!gpsHasFix()) return false;
  double curLat = gps.location.lat();
  double curLon = gps.location.lng();
  outDistanceM = TinyGPSPlus::distanceBetween(curLat, curLon, lat, lon);
  outBearingDeg = TinyGPSPlus::courseTo(curLat, curLon, lat, lon);
  return true;
}

void gpsLogStart() {
  logging = true;
  lastLogMs = 0;
  logPointCount = 0;
}
void gpsLogStop() { logging = false; }
bool gpsLogIsRunning() { return logging; }
uint32_t gpsLogPointCount() { return logPointCount; }

void gpsLogTick() {
  if (!logging || !gpsHasFix()) return;
  unsigned long now = millis();
  if (now - lastLogMs < LOG_INTERVAL_MS) return;
  lastLogMs = now;
  GpsFix f = gpsGetFix();
  char line[128];
  snprintf(line, sizeof(line), "%02u:%02u:%02u,%.6f,%.6f,%.1f,%.1f",
           f.hour, f.minute, f.second, f.lat, f.lon, f.altitudeM, f.speedKmh);
  if (sdAppendLine(TRACK_PATH, String(line))) logPointCount++;
}

bool gpsWaypointSave(const String& name) {
  if (!gpsHasFix()) return false;
  GpsFix f = gpsGetFix();
  String safeName = name;
  safeName.replace(",", " ");
  char line[128];
  snprintf(line, sizeof(line), "%s,%.6f,%.6f,%.1f", safeName.c_str(), f.lat, f.lon, f.altitudeM);
  return sdAppendLine(WAYPOINTS_PATH, String(line));
}

std::vector<GpsWaypoint> gpsWaypointList() {
  std::vector<GpsWaypoint> out;
  String content;
  if (!sdReadFile(WAYPOINTS_PATH, content)) return out;
  int start = 0;
  int len = content.length();
  while (start < len) {
    int nl = content.indexOf('\n', start);
    String line = (nl >= 0) ? content.substring(start, nl) : content.substring(start);
    start = (nl >= 0) ? nl + 1 : len;
    line.trim();
    if (line.length() == 0) continue;
    int c1 = line.indexOf(',');
    int c2 = (c1 >= 0) ? line.indexOf(',', c1 + 1) : -1;
    int c3 = (c2 >= 0) ? line.indexOf(',', c2 + 1) : -1;
    if (c1 < 0 || c2 < 0 || c3 < 0) continue;
    GpsWaypoint w;
    w.name = line.substring(0, c1);
    w.lat = atof(line.substring(c1 + 1, c2).c_str());
    w.lon = atof(line.substring(c2 + 1, c3).c_str());
    w.altitudeM = atof(line.substring(c3 + 1).c_str());
    out.push_back(w);
  }
  return out;
}

bool gpsWaypointDelete(int index) {
  auto list = gpsWaypointList();
  if (index < 0 || index >= (int)list.size()) return false;
  list.erase(list.begin() + index);
  String out = "";
  for (auto& w : list) {
    char line[128];
    snprintf(line, sizeof(line), "%s,%.6f,%.6f,%.1f\n", w.name.c_str(), w.lat, w.lon, w.altitudeM);
    out += line;
  }
  return sdWriteFile(WAYPOINTS_PATH, out);
}

// ====================================================================
//  Wardriving (GPS-tagged WiFi AP logging, WiGLE CSV format)
// ====================================================================
static const char* WARDRIVE_PATH = "/wardrive.csv";
static const unsigned long WARDRIVE_SCAN_INTERVAL_MS = 4000;
static const size_t WARDRIVE_MAX_TRACKED = 4000; // memory guard for the seen-BSSID list
static bool wardriveActive = false;
static uint32_t wardriveNetCount = 0;   // unique networks logged this session
static uint32_t wardriveScanCount = 0;  // scan passes completed this session
static unsigned long lastWardriveScanMs = 0;
static String lastWardriveSsid;
static int lastWardriveRssi = 0;
static String lastWardriveAuth;
static std::vector<String> wardriveSeenBssids;

static String wardriveAuthModeStr(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:            return "[OPEN]";
    case WIFI_AUTH_WEP:             return "[WEP]";
    case WIFI_AUTH_WPA_PSK:         return "[WPA-PSK]";
    case WIFI_AUTH_WPA2_PSK:        return "[WPA2-PSK]";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "[WPA-WPA2-PSK]";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "[WPA2-EAP]";
    case WIFI_AUTH_WPA3_PSK:        return "[WPA3-PSK]";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "[WPA2-WPA3-PSK]";
    default:                        return "[UNKNOWN]";
  }
}

static void wardriveEnsureFile() {
  String content;
  if (!sdReadFile(WARDRIVE_PATH, content) || content.length() == 0) {
    String header = "WigleWifi-1.4,appRelease=1.0,model=" + String(DEVICE_NAME) +
                     ",release=1.0,device=ESP32-S3,display=ILI9341,board=esp32,brand=" +
                     String(DEVICE_NAME) + "\n";
    header += "MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type";
    sdWriteFile(WARDRIVE_PATH, header + "\n");
  }
}

// One passive scan pass: reads currently-visible APs, appends any BSSID we
// haven't logged yet this session (with current GPS fix) to the CSV.
// No probing, no association, no deauth - listen-only.
static void wardriveScanOnce() {
  if (!gpsHasFix()) return; // don't log networks with no usable position

  wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_AP) {
    // Keep the existing AP alive (the webUI itself may be served from
    // it) by adding STA rather than dropping to STA-only. This runs
    // on every wardrive tick while active, so forcing WIFI_STA here
    // would repeatedly tear the AP down for as long as wardriving runs.
    WiFi.mode(WIFI_AP_STA);
  } else if (mode != WIFI_STA && mode != WIFI_AP_STA) {
    WiFi.mode(WIFI_STA);
  }
  int n = WiFi.scanNetworks(false /*async*/, true /*show hidden*/);
  wardriveScanCount++;
  if (n <= 0) { WiFi.scanDelete(); return; }

  GpsFix f = gpsGetFix();
  wardriveEnsureFile();

  char firstSeen[24];
  if (f.year > 0) {
    snprintf(firstSeen, sizeof(firstSeen), "%04u-%02u-%02u %02u:%02u:%02u",
             f.year, f.month, f.day, f.hour, f.minute, f.second);
  } else {
    snprintf(firstSeen, sizeof(firstSeen), "1970-01-01 00:00:00");
  }

  for (int i = 0; i < n; i++) {
    String bssid = WiFi.BSSIDstr(i);
    if (std::find(wardriveSeenBssids.begin(), wardriveSeenBssids.end(), bssid) != wardriveSeenBssids.end()) {
      continue; // already logged this session
    }
    if (wardriveSeenBssids.size() < WARDRIVE_MAX_TRACKED) wardriveSeenBssids.push_back(bssid);

    String ssid = WiFi.SSID(i);
    ssid.replace(",", " ");
    if (ssid.length() == 0) ssid = "[hidden]";

    char line[256];
    snprintf(line, sizeof(line), "%s,%s,%s,%s,%d,%d,%.6f,%.6f,%.1f,%d,WIFI",
             bssid.c_str(), ssid.c_str(), wardriveAuthModeStr(WiFi.encryptionType(i)).c_str(),
             firstSeen, WiFi.channel(i), WiFi.RSSI(i), f.lat, f.lon, f.altitudeM, 0);
    if (sdAppendLine(WARDRIVE_PATH, String(line))) {
      wardriveNetCount++;
      lastWardriveSsid = ssid;
      lastWardriveRssi = WiFi.RSSI(i);
      lastWardriveAuth = wardriveAuthModeStr(WiFi.encryptionType(i));
    }
  }
  WiFi.scanDelete();
}

void gpsWardriveTick() {
  if (!wardriveActive) return;
  unsigned long now = millis();
  if (now - lastWardriveScanMs < WARDRIVE_SCAN_INTERVAL_MS) return;
  lastWardriveScanMs = now;
  wardriveScanOnce();
}

void gpsWardriveStart() {
    if (!wardriveActive) {
        wardriveActive = true;
        wardriveEnsureFile();
        lastWardriveScanMs = 0; // trigger scan immediately
    }
}
void gpsWardriveStop() {
    if (wardriveActive) {
        wardriveActive = false;
        WiFi.scanDelete();
    }
}
bool gpsWardriveIsRunning() { return wardriveActive; }
uint32_t gpsWardriveGetNetCount() { return wardriveNetCount; }

// ====================================================================
//  GPS UI (moved from main.cpp)
// ====================================================================
static SimpleMenu gpsMenu({"Live Fix", "Log Track", "Waypoints", "Wardrive"});
static SimpleMenu gpsWaypointsMenu({});
static int selectedWaypointIndex = -1;
static unsigned long lastGpsLiveRedraw = 0;

// ====================================================================
//  Shared UI toolkit for the screens below
//
//  These are plain RGB565 uint16_t values rather than TFT_xxx macros, so
//  they compile regardless of which color macros display.h happens to
//  expose - swap them for your theme's accent colors if you'd like.
// ====================================================================
static const uint16_t UI_GOOD   = 0x07E0; // green  - good / running / strong
static const uint16_t UI_WARN   = 0xFFE0; // yellow - marginal
static const uint16_t UI_BAD    = 0xF800; // red    - bad / stopped / no fix
static const uint16_t UI_ACCENT = 0x07FF; // cyan   - neutral highlight

// Small rounded status pill, e.g. [ RUNNING ]. ~6px/char at text size 1.
static void uiDrawPill(int x, int y, const char* label, uint16_t color) {
  int w = 12 + (int)strlen(label) * 6;
  int h = 16;
  tft.fillRoundRect(x, y, w, h, h / 2, color);
  tft.setTextSize(1);
  tft.setTextColor(displayColorBg(), color);
  tft.setCursor(x + 6, y + 4);
  tft.print(label);
}

// 4-bar signal-strength glyph (satellite quality or wifi RSSI). lit: 0-4.
static void uiDrawBars(int x, int y, int lit, uint16_t color) {
  const int barW = 4, gap = 2, baseY = y + 14;
  for (int i = 0; i < 4; i++) {
    int h = 4 + i * 4; // 4,8,12,16
    int bx = x + i * (barW + gap);
    uint16_t c = (i < lit) ? color : displayColorFgDim();
    tft.fillRect(bx, baseY - h, barW, h, c);
  }
}

// Labeled stat card: small dim title, bold value, accent stripe on the left.
static void uiDrawCard(int x, int y, int w, int h, const char* title, const String& value, uint16_t accent) {
  tft.drawRoundRect(x, y, w, h, 4, displayColorFgDim());
  tft.fillRect(x + 1, y + 1, 3, h - 2, accent);
  tft.setTextSize(1);
  tft.setTextColor(displayColorFgDim(), displayColorBg());
  tft.setCursor(x + 10, y + 6);
  tft.print(title);
  tft.setTextSize(2);
  tft.setTextColor(displayColorFg(), displayColorBg());
  tft.setCursor(x + 10, y + 20);
  tft.print(value);
}

// Compass ring with a triangular needle pointing at bearingDeg (0 = up/N).
static void uiDrawCompass(int cx, int cy, int r, double bearingDeg, uint16_t color) {
  tft.drawCircle(cx, cy, r, displayColorFgDim());
  double rad = bearingDeg * PI / 180.0;
  int tipX = cx + (int)(sin(rad) * r);
  int tipY = cy - (int)(cos(rad) * r);
  int baseX = cx - (int)(sin(rad) * (r * 0.45));
  int baseY = cy + (int)(cos(rad) * (r * 0.45));
  int leftX  = baseX + (int)(sin(rad + PI / 2) * (r * 0.35));
  int leftY  = baseY - (int)(cos(rad + PI / 2) * (r * 0.35));
  int rightX = baseX + (int)(sin(rad - PI / 2) * (r * 0.35));
  int rightY = baseY - (int)(cos(rad - PI / 2) * (r * 0.35));
  tft.fillTriangle(tipX, tipY, leftX, leftY, rightX, rightY, color);
}

// GPS fix quality -> bar count / color, from satellite count + HDOP.
static int uiFixQualityBars(int sats, double hdop) {
  if (sats >= 7 && hdop <= 1.5) return 4;
  if (sats >= 5 && hdop <= 2.5) return 3;
  if (sats >= 3 && hdop <= 5.0) return 2;
  if (sats >= 1) return 1;
  return 0;
}
static uint16_t uiFixQualityColor(int sats, double hdop) {
  int bars = uiFixQualityBars(sats, hdop);
  if (bars >= 4) return UI_GOOD;
  if (bars >= 2) return UI_WARN;
  return UI_BAD;
}

// Wifi RSSI (dBm) -> bar count / color.
static int uiRssiBars(int rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}
static uint16_t uiRssiColor(int rssi) {
  if (rssi >= -60) return UI_GOOD;
  if (rssi >= -75) return UI_WARN;
  return UI_BAD;
}
static unsigned long lastGpsLogRedraw = 0;

static void drawGpsLive() {
  GpsFix f = gpsGetFix();
  tft.fillScreen(displayColorBg());
  displayDrawHeaderBar("GPS Live");
  int top = displayHeaderHeight() + 8;

  // Status row: fix pill + satellite signal bars + HDOP readout
  uiDrawPill(8, top, f.valid ? "FIX OK" : "SEARCHING", f.valid ? UI_GOOD : UI_BAD);
  int bars = uiFixQualityBars(f.satellites, f.hdop);
  uint16_t qColor = uiFixQualityColor(f.satellites, f.hdop);
  uiDrawBars(146, top + 1, bars, qColor);
  tft.setTextSize(1);
  tft.setTextColor(displayColorFgDim(), displayColorBg());
  tft.setCursor(178, top + 4);
  tft.print(String(f.satellites) + " sat  HDOP " + String(f.hdop, 1));

  int y = top + 26;
  const int gap = 8;
  const int cardW = (tft.width() - 8 - 8 - gap) / 2;

  if (f.valid) {
    uiDrawCard(8, y, cardW, 34, "LATITUDE", String(f.lat, 6), UI_ACCENT);
    uiDrawCard(8 + cardW + gap, y, cardW, 34, "LONGITUDE", String(f.lon, 6), UI_ACCENT);
    y += 34 + gap;
    uiDrawCard(8, y, cardW, 34, "ALTITUDE", String(f.altitudeM, 1) + " m", UI_ACCENT);
    uiDrawCard(8 + cardW + gap, y, cardW, 34, "SPEED", String(f.speedKmh, 1) + " km/h", UI_ACCENT);
    y += 34 + gap;

    // Course compass + numeric heading
    int cx = 8 + 20, cy = y + 20;
    uiDrawCompass(cx, cy, 18, f.courseDeg, UI_ACCENT);
    tft.setTextSize(1);
    tft.setTextColor(displayColorFgDim(), displayColorBg());
    tft.setCursor(48, y + 4);
    tft.print("COURSE");
    tft.setTextSize(2);
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(48, y + 16);
    tft.print(String(f.courseDeg, 0) + " deg");
    y += 44;

    if (f.year > 0) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%02u:%02u:%02u UTC   %02u/%02u/%04u",
               f.hour, f.minute, f.second, f.day, f.month, f.year);
      tft.setTextSize(1);
      tft.setTextColor(displayColorFgDim(), displayColorBg());
      tft.setCursor(8, y);
      tft.print(buf);
    }
  } else {
    tft.setTextSize(1);
    tft.setTextColor(displayColorFgDim(), displayColorBg());
    tft.setCursor(8, y + 6);
    tft.print("Waiting for satellites...");
  }

  tft.setTextColor(displayColorFgDim(), displayColorBg());
  tft.setCursor(8, tft.height() - 14);
  tft.print("OK=save waypoint  BACK=exit");
}

static void drawGpsLog() {
  tft.fillScreen(displayColorBg());
  displayDrawHeaderBar("GPS Log Track");
  int top = displayHeaderHeight() + 10;

  bool running = gpsLogIsRunning();
  // Redraw cadence is ~1s (see gpsHandleLogEvent), so this toggles cleanly.
  bool blinkOn = (millis() / 1000) % 2 == 0;
  uint16_t dotColor = running ? (blinkOn ? UI_GOOD : displayColorFgDim()) : UI_BAD;
  tft.fillCircle(15, top + 7, 5, dotColor);
  tft.setTextSize(1);
  tft.setTextColor(displayColorFg(), displayColorBg());
  tft.setCursor(30, top + 2);
  tft.print(running ? "LOGGING ACTIVE" : "LOGGING STOPPED");

  int y = top + 24;
  uiDrawCard(8, y, tft.width() - 16, 40, "POINTS LOGGED", String(gpsLogPointCount()), UI_ACCENT);
  y += 40 + 10;
  uiDrawPill(8, y, gpsHasFix() ? "GPS FIX OK" : "NO FIX", gpsHasFix() ? UI_GOOD : UI_BAD);

  tft.setTextColor(displayColorFgDim(), displayColorBg());
  tft.setCursor(8, tft.height() - 28);
  tft.print("OK=start/stop");
  tft.setCursor(8, tft.height() - 14);
  tft.print("Saved to /gps_track.csv");
}

static void drawGpsWaypoints() {
  displayShowMenu("Waypoints (OK=view RIGHT=del)", gpsWaypointsMenu.items(), gpsWaypointsMenu.index());
}

static void drawGpsWaypointView() {
  auto list = gpsWaypointList();
  tft.fillScreen(displayColorBg());
  if (selectedWaypointIndex < 0 || selectedWaypointIndex >= (int)list.size()) {
    displayDrawHeaderBar("Waypoint");
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(8, displayHeaderHeight() + 10);
    tft.print("Not found");
    return;
  }
  GpsWaypoint w = list[selectedWaypointIndex];
  displayDrawHeaderBar(w.name.c_str());
  int top = displayHeaderHeight() + 10;
  const int gap = 8;
  const int cardW = (tft.width() - 8 - 8 - gap) / 2;

  uiDrawCard(8, top, cardW, 34, "LATITUDE", String(w.lat, 6), UI_ACCENT);
  uiDrawCard(8 + cardW + gap, top, cardW, 34, "LONGITUDE", String(w.lon, 6), UI_ACCENT);
  int y = top + 34 + gap;
  uiDrawCard(8, y, cardW, 34, "ALTITUDE", String(w.altitudeM, 1) + " m", UI_ACCENT);

  double dist, brg;
  if (gpsBearingTo(w.lat, w.lon, dist, brg)) {
    uiDrawCard(8 + cardW + gap, y, cardW, 34, "DISTANCE",
               dist >= 1000 ? String(dist / 1000.0, 2) + " km" : String(dist, 0) + " m", UI_GOOD);
    y += 34 + gap;
    int cx = 8 + 20, cy = y + 20;
    uiDrawCompass(cx, cy, 18, brg, UI_ACCENT);
    tft.setTextSize(1);
    tft.setTextColor(displayColorFgDim(), displayColorBg());
    tft.setCursor(48, y + 4);
    tft.print("BEARING");
    tft.setTextSize(2);
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(48, y + 16);
    tft.print(String(brg, 0) + " deg");
  } else {
    uiDrawPill(8 + cardW + gap, y + 8, "NO FIX", UI_BAD);
  }

  tft.setTextColor(displayColorFgDim(), displayColorBg());
  tft.setCursor(8, tft.height() - 14);
  tft.print("OK=refresh  BACK=exit");
}

static unsigned long lastGpsWardriveRedraw = 0;

static void drawGpsWardrive() {
  tft.fillScreen(displayColorBg());
  displayDrawHeaderBar("Wardriving");
  int top = displayHeaderHeight() + 8;

  // Status pills: scanning state (blinks while active) + GPS fix
  bool blinkOn = (millis() / 800) % 2 == 0;
  uint16_t scanColor = wardriveActive ? (blinkOn ? UI_GOOD : displayColorFgDim()) : UI_BAD;
  uiDrawPill(8, top, wardriveActive ? "SCANNING" : "STOPPED", scanColor);
  bool hasFix = gpsHasFix();
  uiDrawPill(114, top, hasFix ? "GPS OK" : "NO FIX", hasFix ? UI_GOOD : UI_BAD);

  int y = top + 26;
  const int gap = 8;
  const int cardW = (tft.width() - 8 - 8 - gap) / 2;
  uiDrawCard(8, y, cardW, 36, "NETWORKS", String(wardriveNetCount), UI_ACCENT);
  uiDrawCard(8 + cardW + gap, y, cardW, 36, "SCAN PASSES", String(wardriveScanCount), UI_ACCENT);
  y += 36 + 10;

  // Last network seen: signal bars, SSID, auth mode + RSSI
  tft.setTextSize(1);
  tft.setTextColor(displayColorFgDim(), displayColorBg());
  tft.setCursor(8, y);
  tft.print("LAST SEEN");
  y += 12;
  if (lastWardriveSsid.length() > 0) {
    uiDrawBars(8, y, uiRssiBars(lastWardriveRssi), uiRssiColor(lastWardriveRssi));
    String ssidLine = lastWardriveSsid;
    if (ssidLine.length() > 18) ssidLine = ssidLine.substring(0, 17) + "~";
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(36, y + 3);
    tft.print(ssidLine);
    tft.setTextColor(displayColorFgDim(), displayColorBg());
    tft.setCursor(36, y + 15);
    tft.print(lastWardriveAuth + "  " + String(lastWardriveRssi) + " dBm");
    y += 32;
  } else {
    tft.setTextColor(displayColorFgDim(), displayColorBg());
    tft.setCursor(8, y);
    tft.print("(none yet)");
    y += 16;
  }

  // Passive-only trust badge
  tft.drawRoundRect(8, y, tft.width() - 16, 18, 4, displayColorFgDim());
  tft.setTextColor(displayColorFgDim(), displayColorBg());
  tft.setCursor(14, y + 5);
  tft.print("Passive scan only - no probes/deauth");

  tft.setTextColor(displayColorFgDim(), displayColorBg());
  tft.setCursor(8, tft.height() - 14);
  tft.print("OK=start/stop   Saved to /wardrive.csv");
}

// ---- Public entry points ----
void gpsEnterMenu() {
    gpsInit();
    displayShowMenu("GPS", gpsMenu.items(), gpsMenu.index());
}

void gpsHandleMenuEvent(int evt) {
  if (evt == EVT_UP)   { gpsMenu.up();   gpsEnterMenu(); }
  if (evt == EVT_DOWN) { gpsMenu.down(); gpsEnterMenu(); }
  if (evt == EVT_OK) {
    switch (gpsMenu.index()) {
      case 0: enterState(STATE_GPS_LIVE);      break;
      case 1: enterState(STATE_GPS_LOG);       break;
      case 2: enterState(STATE_GPS_WAYPOINTS); break;
      case 3: enterState(STATE_GPS_WARDRIVE);  break;
    }
  }
}

void gpsEnterLive() {
  lastGpsLiveRedraw = 0;
  drawGpsLive();
}

void gpsHandleLiveEvent(int evt) {
  if (evt == EVT_OK) {
    GpsFix f = gpsGetFix();
    char name[24];
    snprintf(name, sizeof(name), "WP_%02u%02u%02u", f.hour, f.minute, f.second);
    bool ok = gpsWaypointSave(String(name));
    displayShowMessage("GPS", ok ? ("Saved as " + String(name)).c_str() : "No fix yet / SD error");
    delay(600);
    gpsEnterLive();
  }
  unsigned long now = millis();
  if (now - lastGpsLiveRedraw > 500) {
    lastGpsLiveRedraw = now;
    drawGpsLive();
  }
}

void gpsEnterLog() {
  lastGpsLogRedraw = 0;
  drawGpsLog();
}

void gpsHandleLogEvent(int evt) {
  if (evt == EVT_OK) {
    if (gpsLogIsRunning()) gpsLogStop(); else gpsLogStart();
    drawGpsLog();
  }
  unsigned long now = millis();
  if (now - lastGpsLogRedraw > 1000) {
    lastGpsLogRedraw = now;
    drawGpsLog();
  }
}

void gpsEnterWaypoints() {
  auto list = gpsWaypointList();
  std::vector<String> labels;
  for (auto &w : list) labels.push_back(w.name);
  if (labels.empty()) labels.push_back("(no waypoints saved)");
  gpsWaypointsMenu.setItems(labels);
  gpsWaypointsMenu.setIndex(0);
  drawGpsWaypoints();
}

void gpsHandleWaypointsEvent(int evt) {
  if (evt == EVT_UP)   { gpsWaypointsMenu.up();   drawGpsWaypoints(); }
  if (evt == EVT_DOWN) { gpsWaypointsMenu.down(); drawGpsWaypoints(); }
  if (evt == EVT_OK) {
    auto list = gpsWaypointList();
    int idx = gpsWaypointsMenu.index();
    if (idx < (int)list.size()) {
      selectedWaypointIndex = idx;
      enterState(STATE_GPS_WAYPOINT_VIEW);
    }
  }
  if (evt == EVT_RIGHT) {
    auto list = gpsWaypointList();
    int idx = gpsWaypointsMenu.index();
    if (idx < (int)list.size()) {
      gpsWaypointDelete(idx);
      gpsEnterWaypoints();
    }
  }
}

void gpsEnterWaypointView() {
  drawGpsWaypointView();
}

void gpsHandleWaypointViewEvent(int evt) {
  if (evt == EVT_OK) drawGpsWaypointView();
}

void gpsEnterWardrive() {
  lastGpsWardriveRedraw = 0;
  lastWardriveScanMs = 0; // scan almost immediately on entry if active
  drawGpsWardrive();
}

void gpsHandleWardriveEvent(int evt) {
  if (evt == EVT_OK) {
    wardriveActive = !wardriveActive;
    if (wardriveActive) {
      wardriveEnsureFile();
      lastWardriveScanMs = 0; // trigger a scan right away
    } else {
      WiFi.scanDelete();
    }
    drawGpsWardrive();
  }

  unsigned long now = millis();
  if (now - lastGpsWardriveRedraw > 800) {
    lastGpsWardriveRedraw = now;
    drawGpsWardrive();
  }
}