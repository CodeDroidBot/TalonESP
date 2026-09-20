#include "wireguard_module.h"
#include <WiFi.h>
#include <WireGuard-ESP32.h>
#include "sdcard.h"
#include "display.h"
#include "buttons.h"
#include "helpers.h"
#include "wifi_module.h"

static WireGuard wg;
static bool wgUp = false;
static bool wgConfigLoaded = false;
static String wgLastError = "";

// ====================================================================
//  Config model + parser
//
//  Previously the parser wrote straight into a pile of file-scope
//  statics as it walked the .conf, so a partially-invalid file could
//  leave old fields mixed with new ones. Parsing is now a pure function
//  that fills a WgConfig value and only commits it to the "current
//  config" state on success - I/O (reading the file) is kept out of the
//  parser entirely so the two can be reasoned about (and reused)
//  separately.
// ====================================================================
struct WgConfig {
  String privateKey;
  IPAddress localIp;
  int prefixLen = 32;
  String publicKey;
  String endpointHost;
  int endpointPort = 51820;
};

static WgConfig wgCfg;
static String wgConfigWarnings = "";

static const char* WG_CONF_PATH = "/wireguard.conf";

static String trimmed(const String& s) {
  String out = s;
  out.trim();
  return out;
}

// Parses "10.0.0.2/24" (or bare "10.0.0.2", treated as /32) into an IP
// and prefix length. /32 matches what this library's simpler begin()
// overload used to hardcode, so an Address line with no prefix behaves
// the same as before.
static bool parseIpAndPrefix(const String& addrCidr, IPAddress& outIp, int& outPrefixLen) {
  int slash = addrCidr.indexOf('/');
  String ipPart = (slash >= 0) ? addrCidr.substring(0, slash) : addrCidr;
  ipPart.trim();
  outPrefixLen = 32;
  if (slash >= 0) {
    String prefixPart = addrCidr.substring(slash + 1);
    prefixPart.trim();
    int p = prefixPart.toInt();
    if (p >= 0 && p <= 32) outPrefixLen = p;
  }
  return outIp.fromString(ipPart);
}

static void addWarning(String& warnings, const char* msg) {
  if (warnings.length() > 0) warnings += "; ";
  warnings += msg;
}

// Pure parse: reads `content` (the raw file text), fills `out` and
// `warnings`. Returns false with `error` set if required fields are
// missing. Does no I/O and touches no global state, so it can't leave
// wgCfg half-updated on a bad file.
static bool parseWgConfigText(const String& content, WgConfig& out, String& warnings, String& error) {
  out = WgConfig(); // start from defaults every parse
  warnings = "";
  error = "";
  bool sawSecondPeer = false;
  String section = "";
  bool haveKey = false, haveAddr = false, havePub = false, haveEndpoint = false;
  int peerSections = 0;

  int start = 0, len = content.length();
  while (start < len) {
    int nl = content.indexOf('\n', start);
    String line = (nl >= 0) ? content.substring(start, nl) : content.substring(start);
    start = (nl >= 0) ? nl + 1 : len;
    line = trimmed(line);
    if (line.length() == 0 || line.startsWith("#") || line.startsWith(";")) continue;

    if (line.startsWith("[")) {
      section = line;
      if (section.equalsIgnoreCase("[Peer]")) {
        peerSections++;
        if (peerSections > 1) sawSecondPeer = true;
      }
      continue;
    }
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String key = trimmed(line.substring(0, eq));
    String val = trimmed(line.substring(eq + 1));
    key.toLowerCase();

    // Only the first [Peer] block is used - see sawSecondPeer below.
    if (section.equalsIgnoreCase("[Interface]")) {
      if (key == "privatekey") { out.privateKey = val; haveKey = true; }
      else if (key == "address") {
        haveAddr = parseIpAndPrefix(val, out.localIp, out.prefixLen);
        if (haveAddr && out.prefixLen != 32) {
          addWarning(warnings, "Address prefix ignored - library always brings up a /32-style single tunnel");
        }
      }
      else if (key == "dns") {
        addWarning(warnings, "DNS line ignored (library doesn't manage DNS)");
      }
      else if (key == "mtu" || key == "table" || key == "postup" || key == "postdown" || key == "preup" || key == "predown") {
        addWarning(warnings, "Interface options besides PrivateKey/Address ignored");
      }
    } else if (section.equalsIgnoreCase("[Peer]") && peerSections == 1) {
      if (key == "publickey") { out.publicKey = val; havePub = true; }
      else if (key == "endpoint") {
        int colon = val.lastIndexOf(':');
        if (colon > 0) {
          out.endpointHost = val.substring(0, colon);
          out.endpointPort = val.substring(colon + 1).toInt();
          haveEndpoint = (out.endpointPort > 0);
        }
      }
      else if (key == "presharedkey") {
        addWarning(warnings, "PresharedKey ignored (unsupported by this library)");
      }
      else if (key == "persistentkeepalive") {
        addWarning(warnings, "PersistentKeepalive ignored - tunnel may go idle behind NAT");
      }
      // AllowedIPs intentionally ignored - this library brings up a
      // single default tunnel, it doesn't do per-route allowed-ip
      // splitting the way wg-quick's `ip route` calls would.
    }
  }

  if (sawSecondPeer) {
    addWarning(warnings, "Only the first [Peer] section is used; extra peers ignored");
  }
  if (!(haveKey && haveAddr && havePub && haveEndpoint)) {
    error = "Incomplete config (need PrivateKey/Address/PublicKey/Endpoint)";
    return false;
  }
  return true;
}

// I/O + commit: reads the file from SD, parses it, and only replaces the
// live wgCfg on success (a bad edit on the SD card leaves the previously
// loaded config in place rather than clobbering it with a half-parsed one).
bool wgLoadConfigFromSD() {
  wgLastError = "";
  String content;
  if (!sdReadFile(WG_CONF_PATH, content)) {
    wgLastError = "No /wireguard.conf on SD";
    wgConfigLoaded = false;
    return false;
  }

  WgConfig parsed;
  String warnings, error;
  bool ok = parseWgConfigText(content, parsed, warnings, error);
  wgConfigWarnings = warnings; // surfaced either way - useful even on failure
  if (!ok) {
    wgLastError = error;
    wgConfigLoaded = false;
    return false;
  }
  wgCfg = parsed;
  wgConfigLoaded = true;
  return true;
}

bool wgStart() {
  if (!wgConfigLoaded && !wgLoadConfigFromSD()) return false;
  if (WiFi.status() != WL_CONNECTED) {
    wgLastError = "WiFi not connected";
    return false;
  }

  // Defensive: if the library thinks an interface is already up (e.g.
  // we got here without going through wgStop() first), tear it down
  // before re-adding it rather than calling begin() on top of it.
  if (wg.is_initialized()) {
    wg.end();
  }

  // Handshake needs a roughly-correct clock; best-effort, non-blocking-ish.
  // Must happen before begin() - once the WG interface becomes the
  // default route, plain internet/NTP access may no longer work.
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  // This fork of WireGuard-ESP32 only exposes one begin() overload:
  //   bool begin(const IPAddress& localIP, const char* privateKey,
  //              const char* remotePeerAddress, const char* remotePeerPublicKey,
  //              uint16_t remotePeerPort);
  // There is no way to hand it a netmask or gateway - it always brings up
  // a single default tunnel internally. wgCfg.prefixLen is kept around
  // purely for display (wgGetSubnetCidr()) and is flagged to the user via
  // wgGetConfigWarnings() when the Address line specifies a prefix other
  // than /32, since that prefix isn't actually enforced.
  bool ok = wg.begin(wgCfg.localIp, wgCfg.privateKey.c_str(), wgCfg.endpointHost.c_str(),
                      wgCfg.publicKey.c_str(), (uint16_t)wgCfg.endpointPort);
  wgUp = ok && wg.is_initialized();
  if (!ok) {
    wgLastError = "wg.begin() failed";
  } else if (!wg.is_initialized()) {
    wgLastError = "wg.begin() returned true but interface didn't come up";
  }
  return wgUp;
}

void wgStop() {
  if (wg.is_initialized()) {
    wg.end();
  }
  wgUp = false;
}

// Note: this only reflects whether the lwIP netif was brought up, not
// whether a WireGuard handshake with the peer has actually completed -
// the library doesn't expose handshake/peer-reachability state to
// Arduino sketches. Treat "up" as "interface configured", not "verified
// connected"; a wrong key or unreachable endpoint can still show this
// as true.
bool wgIsUp() { return wgUp && wg.is_initialized(); }

String wgGetLocalTunnelIp() { return wgCfg.localIp.toString(); }
String wgGetSubnetCidr() { return wgCfg.localIp.toString() + "/" + String(wgCfg.prefixLen); }
String wgGetEndpoint() { return wgCfg.endpointHost + ":" + String(wgCfg.endpointPort); }
String wgGetLastError() { return wgLastError; }
String wgGetConfigWarnings() { return wgConfigWarnings; }

// ====================================================================
//  UI
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
static RedrawTimer wgRedraw;

static void drawWgScreen() {
  tft.fillScreen(displayColorBg());
  displayDrawHeaderBar("WireGuard VPN");
  int y = displayHeaderHeight() + 12;
  tft.setTextColor(displayColorFg(), displayColorBg());
  tft.setTextSize(1);

  tft.setCursor(8, y);
  tft.print(wifiIsConnected() ? ("WiFi: " + wifiGetLocalIpStr()) : "WiFi: not connected");
  y += 16;

  tft.setCursor(8, y);
  tft.print(wgConfigLoaded ? "Config: loaded" : "Config: not loaded");
  y += 16;

  tft.setCursor(8, y);
  // "Iface" not "Tunnel" - this is interface-up, not a confirmed
  // handshake with the peer (the library doesn't expose that).
  tft.print(wgIsUp() ? "Iface: UP" : "Iface: down");
  y += 16;

  if (wgConfigLoaded) {
    tft.setCursor(8, y);
    tft.print("Local: " + wgGetSubnetCidr());
    y += 14;
    tft.setCursor(8, y);
    tft.print("Peer: " + wgGetEndpoint());
    y += 14;
  }

  if (wgGetConfigWarnings().length() > 0) {
    tft.setTextColor(TFT_YELLOW, displayColorBg());
    tft.setCursor(8, y);
    tft.print(wgGetConfigWarnings());
    tft.setTextColor(displayColorFg(), displayColorBg());
    y += 20;
  }

  if (wgGetLastError().length() > 0) {
    tft.setTextColor(TFT_RED, displayColorBg());
    tft.setCursor(8, y + 6);
    tft.print(wgGetLastError());
    y += 20;
  }

  tft.setTextColor(displayColorFgDim(), displayColorBg());
  tft.setCursor(8, tft.height() - 28);
  tft.print("OK: " + String(wgIsUp() ? "disconnect" : "connect"));
  tft.setCursor(8, tft.height() - 14);
  tft.print("LEFT reload  BACK exit  (/wireguard.conf)");
}

void wgEnterMenu() {
  if (!wgConfigLoaded) wgLoadConfigFromSD();
  wgRedraw.reset();
  drawWgScreen();
}

void wgHandleMenuEvent(int evt) {
  if (evt == EVT_OK) {
    if (wgIsUp()) {
      wgStop();
    } else {
      if (!wgConfigLoaded) wgLoadConfigFromSD();
      wgStart();
    }
    drawWgScreen();
  } else if (evt == EVT_LEFT && !wgIsUp()) {
    // Re-read /wireguard.conf from SD without needing to leave this
    // screen or power-cycle - handy after editing the file on a computer.
    wgLoadConfigFromSD();
    drawWgScreen();
  }
  if (wgRedraw.due(1000)) {
    drawWgScreen();
  }
}


