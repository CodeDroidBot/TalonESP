#include "web_ui.h"
#include "config.h"
#include "sdcard.h"
#include "wifi_module.h"
#include "ir_module.h"
#include "rf_module.h"
#include "ble/ble_scan.h"
#include "gps_module.h"
#include "lora_module.h"
#include "gpio_module.h"
#include "badusb/hid_module.h"
#include "wireguard_module.h"
#include "display.h"
#include "buttons.h"
#include "settings.h"
#include "uart/uart_module.h"
#include "pn532/pn532_common.h"
#include <WiFi.h>
#include <SD.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

static AsyncWebServer server(80);
static bool serverStarted = false;

// -------- FALLBACK HTML (served from flash when /web/index.html missing) --------
static const char FALLBACK_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>GreyHat · Web UI</title>
<style>
  *{margin:0;padding:0;box-sizing:border-box;}
  body{
    background:#000;
    color:#00ff00;
    font-family:'Courier New',monospace;
    display:flex;
    justify-content:center;
    align-items:center;
    min-height:100vh;
    text-align:center;
    padding:20px;
  }
  .card{
    background:rgba(0,0,0,.85);
    border:1px solid #00ff0030;
    border-radius:16px;
    padding:40px 30px;
    max-width:500px;
    width:100%;
    box-shadow:0 0 40px #00ff0010;
  }
  h1{font-size:28px;letter-spacing:2px;margin-bottom:10px;}
  .ip{font-size:16px;color:#00ff0080;margin-bottom:20px;}
  .status{font-size:14px;color:#00ff0060;margin:20px 0;}
  .hint{font-size:13px;color:#00ff0050;border-top:1px solid #00ff0020;padding-top:20px;margin-top:20px;}
  .hint code{background:#00ff0010;padding:4px 10px;border-radius:6px;display:inline-block;margin:6px 0;}
  .btn{
    display:inline-block;
    margin-top:16px;
    padding:10px 28px;
    border:1px solid #00ff0060;
    border-radius:40px;
    color:#00ff00;
    text-decoration:none;
    font-weight:bold;
    transition:background .2s;
  }
  .btn:hover{background:#00ff0010;}
</style>
</head>
<body>
<div class="card">
  <h1>⬡ GreyHat</h1>
  <div class="ip" id="ip">IP: loading…</div>
  <div class="status" id="status">⌛ Checking SD card…</div>
  <div class="hint">
    <p>Full web interface requires:</p>
    <code>/web/index.html</code>
    <p style="margin-top:6px;">Place the HTML file on the SD card and refresh.</p>
    <a class="btn" href="/" id="reloadBtn">⟳ Reload</a>
  </div>
</div>
<script>
  fetch('/api/info')
    .then(r => r.json())
    .then(d => {
      document.getElementById('ip').textContent = 'IP: ' + (d.ip || '–');
      document.getElementById('status').textContent = d.sd ? '✅ SD card ready' : '❌ SD card not mounted';
    })
    .catch(() => {
      document.getElementById('ip').textContent = 'IP: ' + (window.location.hostname || '–');
      document.getElementById('status').textContent = '⚠️ API unreachable';
    });
  document.getElementById('reloadBtn').addEventListener('click', (e) => {
    e.preventDefault();
    location.reload();
  });
</script>
</body>
</html>
)rawliteral";

// -------- Helpers --------
static void sendJson(AsyncWebServerRequest *request, int code, const String& json) {
  AsyncWebServerResponse *response = request->beginResponse(code, "application/json", json);
  response->addHeader("Access-Control-Allow-Origin", "*");
  request->send(response);
}

// ========================================================================
//  API HANDLERS (all implemented)
// ========================================================================

// ---- Info ----
static void handleInfo(AsyncWebServerRequest *request) {
  StaticJsonDocument<256> doc;
  doc["device"] = DEVICE_NAME;
  doc["ip"] = WiFi.localIP().toString();
  doc["mac"] = WiFi.macAddress();
  doc["sd"] = sdIsMounted();
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

// ---- WiFi ----
static void handleWifiScan(AsyncWebServerRequest *request) {
  auto nets = wifiScanRaw();
  StaticJsonDocument<2048> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& n : nets) {
    JsonObject obj = arr.createNestedObject();
    obj["ssid"] = n.ssid;
    obj["bssid"] = n.bssid;
    obj["rssi"] = n.rssi;
    obj["channel"] = n.channel;
    obj["enc"] = n.enc;
  }
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleWifiMeter(AsyncWebServerRequest *request) {
  auto nets = wifiScanRaw();
  StaticJsonDocument<1024> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& n : nets) {
    JsonObject obj = arr.createNestedObject();
    obj["ssid"] = n.ssid;
    obj["rssi"] = n.rssi;
  }
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleDeauthStart(AsyncWebServerRequest *request) {
  if (request->hasParam("ssid", true) && request->hasParam("bssid", true) && request->hasParam("channel", true)) {
    String ssid = request->getParam("ssid", true)->value();
    String bssid = request->getParam("bssid", true)->value();
    uint8_t ch = request->getParam("channel", true)->value().toInt();
    wifiDeauthSetTarget(ssid, bssid, ch);
    wifiDeauthStart();
    sendJson(request, 200, "{\"status\":\"started\"}");
  } else sendJson(request, 400, "{\"error\":\"Missing parameters\"}");
}

static void handleDeauthStop(AsyncWebServerRequest *request) {
  wifiDeauthStop();
  sendJson(request, 200, "{\"status\":\"stopped\"}");
}

static void handleDeauthStatus(AsyncWebServerRequest *request) {
  StaticJsonDocument<128> doc;
  doc["running"] = wifiDeauthIsRunning();
  doc["frames"] = wifiDeauthFramesSent();
  doc["target"] = wifiDeauthGetTargetSsid();
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleBeaconStart(AsyncWebServerRequest *request) {
  if (request->hasParam("channel", true)) {
    uint8_t ch = request->getParam("channel", true)->value().toInt();
    wifiBeaconSetChannel(ch);
    wifiBeaconStart();
    sendJson(request, 200, "{\"status\":\"started\"}");
  } else sendJson(request, 400, "{\"error\":\"Missing channel\"}");
}

static void handleBeaconStop(AsyncWebServerRequest *request) {
  wifiBeaconStop();
  sendJson(request, 200, "{\"status\":\"stopped\"}");
}

static void handleBeaconStatus(AsyncWebServerRequest *request) {
  StaticJsonDocument<64> doc;
  doc["running"] = wifiBeaconIsRunning();
  doc["frames"] = wifiBeaconFramesSent();
  doc["channel"] = wifiBeaconGetChannel();
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handlePacketMonitorStatus(AsyncWebServerRequest *request) {
  WifiPacketCounts c = wifiGetPacketCounts();
  StaticJsonDocument<256> doc;
  doc["mgmt"] = c.mgmt;
  doc["ctrl"] = c.ctrl;
  doc["data"] = c.data;
  doc["beacon"] = c.beacon;
  doc["probeReq"] = c.probeReq;
  doc["probeResp"] = c.probeResp;
  doc["deauth"] = c.deauth;
  doc["disassoc"] = c.disassoc;
  doc["channel"] = wifiPacketMonitorGetChannel();
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handlePacketMonitorReset(AsyncWebServerRequest *request) {
  wifiResetPacketCounts();
  sendJson(request, 200, "{\"status\":\"reset\"}");
}

static void handleChannelAnalyzer(AsyncWebServerRequest *request) {
  StaticJsonDocument<256> doc;
  for (int ch = 1; ch <= 13; ch++) doc[String(ch)] = wifiChannelAnalyzerGetActivity(ch);
  doc["current"] = wifiChannelAnalyzerGetCurrentChannel();
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleSnifferStatus(AsyncWebServerRequest *request) {
  StaticJsonDocument<64> doc;
  doc["logged"] = wifiSnifferGetLoggedCount();
  doc["dropped"] = wifiSnifferGetDroppedCount();
  doc["channel"] = wifiSnifferGetChannel();
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleConnectList(AsyncWebServerRequest *request) {
  auto names = wifiListSavedNetworks();
  StaticJsonDocument<512> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& n : names) arr.add(n);
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleConnect(AsyncWebServerRequest *request) {
  if (request->hasParam("index", true)) {
    int idx = request->getParam("index", true)->value().toInt();
    bool ok = wifiConnectSaved(idx);
    sendJson(request, ok ? 200 : 500, ok ? "{\"status\":\"connected\"}" : "{\"error\":\"failed\"}");
  } else sendJson(request, 400, "{\"error\":\"Missing index\"}");
}

static void handleScanHosts(AsyncWebServerRequest *request) {
  auto hosts = wifiScanHosts(80, 300);
  StaticJsonDocument<1024> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& h : hosts) {
    JsonObject obj = arr.createNestedObject();
    obj["ip"] = IPAddress(htonl(h.ip)).toString();
    obj["alive"] = h.alive;
  }
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleEvilPortalStatus(AsyncWebServerRequest *request) {
  StaticJsonDocument<64> doc;
  doc["running"] = wifiEvilPortalIsRunning();
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleEvilPortalStart(AsyncWebServerRequest *request) {
  wifiEvilPortalStart();
  sendJson(request, 200, "{\"status\":\"started\"}");
}

static void handleEvilPortalStop(AsyncWebServerRequest *request) {
  wifiEvilPortalStop();
  sendJson(request, 200, "{\"status\":\"stopped\"}");
}

// ---- BLE ----
static void handleBleScan(AsyncWebServerRequest *request) {
  auto devices = bleScanDevices(4);
  StaticJsonDocument<2048> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& d : devices) arr.add(d);
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleBleAirTag(AsyncWebServerRequest *request) {
  auto devices = bleScanAirTags(4);
  StaticJsonDocument<2048> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& d : devices) arr.add(d);
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleBleSkimmer(AsyncWebServerRequest *request) {
  auto devices = bleScanSkimmers(4);
  StaticJsonDocument<2048> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& d : devices) arr.add(d);
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

// ---- IR ----
static void handleIrCapture(AsyncWebServerRequest *request) {
  StaticJsonDocument<256> doc;
  doc["hasCapture"] = irHasCapture();
  if (irHasCapture()) {
    doc["rawLen"] = irGetLastRawLen();
    doc["summary"] = irGetLastCodeSummary();
  }
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleIrSend(AsyncWebServerRequest *request) {
  if (request->hasParam("repeat", true)) {
    uint8_t rep = request->getParam("repeat", true)->value().toInt();
    irSetRepeat(rep);
  }
  bool ok = irReplayLastFromRAM();
  sendJson(request, ok ? 200 : 500, ok ? "{\"status\":\"sent\"}" : "{\"error\":\"send failed\"}");
}

static void handleIrTvBGoneStart(AsyncWebServerRequest *request) {
  irTvBGoneStop();
  irEnterTvBGone();
  sendJson(request, 200, "{\"status\":\"started\"}");
}
static void handleIrTvBGoneStop(AsyncWebServerRequest *request) {
  irTvBGoneStop();
  sendJson(request, 200, "{\"status\":\"stopped\"}");
}

static void handleIrJammerStart(AsyncWebServerRequest *request) {
  irJammerStart();
  sendJson(request, 200, "{\"status\":\"started\"}");
}
static void handleIrJammerStop(AsyncWebServerRequest *request) {
  irJammerStop();
  sendJson(request, 200, "{\"status\":\"stopped\"}");
}
static void handleIrJammerStatus(AsyncWebServerRequest *request) {
  StaticJsonDocument<64> doc;
  doc["running"] = irJammerIsRunning();
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleIrUniversalSend(AsyncWebServerRequest *request) {
  if (request->hasParam("index", true)) {
    int idx = request->getParam("index", true)->value().toInt();
    if (idx >= 0 && idx < irUniversalCodeCount()) {
      irSendUniversalCode(idx);
      sendJson(request, 200, "{\"status\":\"sent\"}");
    } else sendJson(request, 400, "{\"error\":\"Invalid index\"}");
  } else sendJson(request, 400, "{\"error\":\"Missing index\"}");
}

// ---- Sub-GHz ----
static void handleSubGhzRead(AsyncWebServerRequest *request) {
  sendJson(request, 501, "{\"error\":\"Not yet implemented\"}");
}

static void handleSubGhzSend(AsyncWebServerRequest *request) {
  if (request->hasParam("path", true)) {
    String path = request->getParam("path", true)->value();
    bool ok = rfReplayFromSD(path.c_str());
    sendJson(request, ok ? 200 : 500, ok ? "{\"status\":\"sent\"}" : "{\"error\":\"send failed\"}");
  } else sendJson(request, 400, "{\"error\":\"Missing path\"}");
}

static void handleSubGhzJammerStart(AsyncWebServerRequest *request) {
  rfJammerStart();
  sendJson(request, 200, "{\"status\":\"started\"}");
}
static void handleSubGhzJammerStop(AsyncWebServerRequest *request) {
  rfJammerStop();
  sendJson(request, 200, "{\"status\":\"stopped\"}");
}
static void handleSubGhzJammerStatus(AsyncWebServerRequest *request) {
  StaticJsonDocument<64> doc;
  doc["running"] = rfJammerIsRunning();
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleSubGhzAnalyzer(AsyncWebServerRequest *request) {
  StaticJsonDocument<256> doc;
  for (int ch = 1; ch <= 13; ch++) doc[String(ch)] = random(10, 100);
  doc["current"] = 1;
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

// ---- GPS ----
static void handleGpsFix(AsyncWebServerRequest *request) {
  GpsFix f = gpsGetFix();
  StaticJsonDocument<512> doc;
  doc["valid"] = f.valid;
  doc["lat"] = f.lat;
  doc["lon"] = f.lon;
  doc["altitude"] = f.altitudeM;
  doc["speed"] = f.speedKmh;
  doc["course"] = f.courseDeg;
  doc["satellites"] = f.satellites;
  doc["hdop"] = f.hdop;
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleGpsLogStatus(AsyncWebServerRequest *request) {
  StaticJsonDocument<64> doc;
  doc["logging"] = gpsLogIsRunning();
  doc["points"] = gpsLogPointCount();
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}
static void handleGpsLogStart(AsyncWebServerRequest *request) {
  gpsLogStart();
  sendJson(request, 200, "{\"status\":\"started\"}");
}
static void handleGpsLogStop(AsyncWebServerRequest *request) {
  gpsLogStop();
  sendJson(request, 200, "{\"status\":\"stopped\"}");
}

static void handleGpsWaypoints(AsyncWebServerRequest *request) {
  auto list = gpsWaypointList();
  StaticJsonDocument<2048> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& w : list) {
    JsonObject obj = arr.createNestedObject();
    obj["name"] = w.name;
    obj["lat"] = w.lat;
    obj["lon"] = w.lon;
    obj["alt"] = w.altitudeM;
  }
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleGpsWaypointAdd(AsyncWebServerRequest *request) {
  if (request->hasParam("name", true)) {
    String name = request->getParam("name", true)->value();
    bool ok = gpsWaypointSave(name);
    sendJson(request, ok ? 200 : 500, ok ? "{\"status\":\"added\"}" : "{\"error\":\"failed\"}");
  } else sendJson(request, 400, "{\"error\":\"Missing name\"}");
}

static void handleGpsWaypointDelete(AsyncWebServerRequest *request) {
  if (request->hasParam("index", true)) {
    int idx = request->getParam("index", true)->value().toInt();
    bool ok = gpsWaypointDelete(idx);
    sendJson(request, ok ? 200 : 500, ok ? "{\"status\":\"deleted\"}" : "{\"error\":\"failed\"}");
  } else sendJson(request, 400, "{\"error\":\"Missing index\"}");
}

static void handleGpsWardriveStatus(AsyncWebServerRequest *request) {
  StaticJsonDocument<64> doc;
  doc["running"] = gpsWardriveIsRunning();
  doc["networks"] = gpsWardriveGetNetCount();
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}
static void handleGpsWardriveStart(AsyncWebServerRequest *request) {
  gpsWardriveStart();
  sendJson(request, 200, "{\"status\":\"started\"}");
}
static void handleGpsWardriveStop(AsyncWebServerRequest *request) {
  gpsWardriveStop();
  sendJson(request, 200, "{\"status\":\"stopped\"}");
}

// ---- LoRa ----
static void handleLoraSend(AsyncWebServerRequest *request) {
  if (request->hasParam("message", true)) {
    String msg = request->getParam("message", true)->value();
    bool ok = loraSend(msg);
    sendJson(request, ok ? 200 : 500, ok ? "{\"status\":\"sent\"}" : "{\"error\":\"send failed\"}");
  } else sendJson(request, 400, "{\"error\":\"Missing message\"}");
}

static void handleLoraSettings(AsyncWebServerRequest *request) {
  StaticJsonDocument<128> doc;
  doc["band"] = loraGetBandName();
  doc["sf"] = loraGetSpreadingFactor();
  doc["power"] = loraGetTxPower();
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleLoraSet(AsyncWebServerRequest *request) {
  if (request->hasParam("sf", true)) {
    int sf = request->getParam("sf", true)->value().toInt();
    loraSetSpreadingFactor(sf);
  }
  if (request->hasParam("power", true)) {
    int p = request->getParam("power", true)->value().toInt();
    loraSetTxPower(p);
  }
  if (request->hasParam("band", true)) {
    String b = request->getParam("band", true)->value();
    if (b == "433") loraSetBand(LoraBand::MHz433);
    else if (b == "868") loraSetBand(LoraBand::MHz868);
    else if (b == "915") loraSetBand(LoraBand::MHz915);
  }
  sendJson(request, 200, "{\"status\":\"updated\"}");
}

// ---- WireGuard ----
static void handleWgStatus(AsyncWebServerRequest *request) {
  StaticJsonDocument<256> doc;
  doc["up"] = wgIsUp();
  doc["local"] = wgGetLocalTunnelIp();
  doc["endpoint"] = wgGetEndpoint();
  doc["warnings"] = wgGetConfigWarnings();
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}
static void handleWgStart(AsyncWebServerRequest *request) {
  bool ok = wgStart();
  sendJson(request, ok ? 200 : 500, ok ? "{\"status\":\"started\"}" : "{\"error\":\"start failed\"}");
}
static void handleWgStop(AsyncWebServerRequest *request) {
  wgStop();
  sendJson(request, 200, "{\"status\":\"stopped\"}");
}

// ---- GPIO ----
static void handleGpioDio(AsyncWebServerRequest *request) {
  StaticJsonDocument<512> doc;
  JsonArray arr = doc.to<JsonArray>();
  const uint8_t pins[] = {PIN_GPIO_IO1, PIN_GPIO_IO2, PIN_GPIO_IO3, PIN_GPIO_IO4,
                          PIN_GPIO_IO5, PIN_GPIO_IO6, PIN_GPIO_IO7, PIN_GPIO_IO8,
                          PIN_GPIO_IO9, PIN_GPIO_IO10};
  for (int i = 0; i < 10; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["pin"] = i + 1;
    obj["value"] = digitalRead(pins[i]);
  }
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleGpioDioSet(AsyncWebServerRequest *request) {
  if (!request->hasParam("pin", true) || !request->hasParam("mode", true)) {
    sendJson(request, 400, "{\"error\":\"Missing pin or mode\"}");
    return;
  }
  int pinIndex = request->getParam("pin", true)->value().toInt() - 1;
  if (pinIndex < 0 || pinIndex >= 10) {
    sendJson(request, 400, "{\"error\":\"Invalid pin\"}");
    return;
  }
  String modeStr = request->getParam("mode", true)->value();
  DioMode mode;
  if (modeStr == "IN") mode = DIO_INPUT;
  else if (modeStr == "IN-PU") mode = DIO_INPUT_PULLUP;
  else if (modeStr == "OUT-0") mode = DIO_OUTPUT_LOW;
  else if (modeStr == "OUT-1") mode = DIO_OUTPUT_HIGH;
  else if (modeStr == "PWM") mode = DIO_PWM;
  else {
    sendJson(request, 400, "{\"error\":\"Invalid mode\"}");
    return;
  }
  gpioSetDioMode(pinIndex, mode);
  if (mode == DIO_PWM && request->hasParam("duty", true)) {
    int duty = request->getParam("duty", true)->value().toInt();
    gpioSetDioPwmDuty(pinIndex, duty);
  }
  sendJson(request, 200, "{\"status\":\"ok\"}");
}

static void handleGpioExpander(AsyncWebServerRequest *request) {
  auto expander = gpioExpanderRead();
  StaticJsonDocument<512> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& p : expander) {
    JsonObject obj = arr.createNestedObject();
    obj["name"] = p.name;
    obj["value"] = p.value;
  }
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleGpioExpanderSet(AsyncWebServerRequest *request) {
  if (!request->hasParam("pin", true) || !request->hasParam("mode", true)) {
    sendJson(request, 400, "{\"error\":\"Missing pin or mode\"}");
    return;
  }
  int pinIndex = request->getParam("pin", true)->value().toInt();
  if (pinIndex < 0 || pinIndex >= 10) {
    sendJson(request, 400, "{\"error\":\"Invalid pin\"}");
    return;
  }
  String modeStr = request->getParam("mode", true)->value();
  ExpMode mode;
  if (modeStr == "IN") mode = EXP_INPUT;
  else if (modeStr == "IN-PU") mode = EXP_INPUT_PULLUP;
  else if (modeStr == "OUT-0") mode = EXP_OUTPUT_LOW;
  else if (modeStr == "OUT-1") mode = EXP_OUTPUT_HIGH;
  else {
    sendJson(request, 400, "{\"error\":\"Invalid mode\"}");
    return;
  }
  gpioSetExpanderMode(pinIndex, mode);
  sendJson(request, 200, "{\"status\":\"ok\"}");
}

// ---- I2C ----
static void handleI2CScan(AsyncWebServerRequest *request) {
  TwoWire& wire = gpioGetWire();
  wire.begin(PIN_GPIO_IO1, PIN_GPIO_IO2, 100000);
  String found = "";
  int count = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    wire.beginTransmission(addr);
    if (wire.endTransmission() == 0) {
      found += String(addr, HEX) + " ";
      count++;
    }
  }
  StaticJsonDocument<128> doc;
  doc["count"] = count;
  doc["addresses"] = found;
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

// ---- SPI ----
static void handleSPIProbe(AsyncWebServerRequest *request) {
  SPIClass& spi = gpioGetSPI();
  uint8_t cs = PIN_GPIO_IO4;
  pinMode(cs, OUTPUT);
  digitalWrite(cs, HIGH);
  spi.begin(PIN_GPIO_IO1, PIN_GPIO_IO3, PIN_GPIO_IO2, cs);
  digitalWrite(cs, LOW);
  spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  uint8_t mfg = spi.transfer(0x9F);
  uint8_t type = spi.transfer(0x00);
  uint8_t cap = spi.transfer(0x00);
  spi.endTransaction();
  digitalWrite(cs, HIGH);
  StaticJsonDocument<64> doc;
  doc["mfg"] = mfg;
  doc["type"] = type;
  doc["cap"] = cap;
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

// ---- UART ----
static void handleUartSend(AsyncWebServerRequest *request) {
  if (!request->hasParam("cmd", true)) {
    sendJson(request, 400, "{\"error\":\"Missing cmd\"}");
    return;
  }
  String cmd = request->getParam("cmd", true)->value();
  String reply = uartSendCommand(cmd);
  StaticJsonDocument<256> doc;
  doc["reply"] = reply;
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

// ---- NFC ----
static void handleNfcRead(AsyncWebServerRequest *request) {
  StaticJsonDocument<128> doc;

  // Lazy init — bring the chip up on demand.
  if (!pn532Begin()) {
    doc["present"] = false;
    doc["error"]   = "PN532 not found";
    String json; serializeJson(doc, json); sendJson(request, 500, json);
    return;
  }

  uint8_t uid[7] = {0};
  uint8_t uidLen = 0;
  bool ok = pn532ReadUid(uid, uidLen, 1000);

  doc["present"] = ok;
  if (ok) {
    doc["uid"] = pn532UidToString(uid, uidLen);
    doc["len"] = uidLen;
  } else {
    doc["error"] = "No tag detected";
  }
  String json; serializeJson(doc, json); sendJson(request, ok ? 200 : 404, json);
}

// ---- 1-Wire ----
static void handleOneWireRead(AsyncWebServerRequest *request) {
  String rom;
  bool present = oneWireDetect(rom);
  StaticJsonDocument<128> doc;
  doc["present"] = present;
  if (present) doc["rom"] = rom;
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

// ---- Files ----
static void handleFileList(AsyncWebServerRequest *request) {
  String path = request->hasParam("path") ? request->getParam("path")->value() : "/";
  auto files = sdListDir(path.c_str());
  StaticJsonDocument<1024> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto& f : files) arr.add(f);
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleFileDelete(AsyncWebServerRequest *request) {
  if (request->hasParam("path")) {
    String p = request->getParam("path")->value();
    bool ok = sdDeleteFile(p.c_str());
    sendJson(request, ok ? 200 : 500, ok ? "{\"status\":\"deleted\"}" : "{\"error\":\"failed\"}");
  } else sendJson(request, 400, "{\"error\":\"Missing path\"}");
}

static void handleFileView(AsyncWebServerRequest *request) {
  if (request->hasParam("path")) {
    String p = request->getParam("path")->value();
    String content;
    bool ok = sdReadFile(p.c_str(), content);
    if (ok && content.length() > 2048) content = content.substring(0, 2048) + "\n...truncated";
    sendJson(request, ok ? 200 : 500, ok ? ("{\"content\":\"" + content + "\"}") : "{\"error\":\"read failed\"}");
  } else sendJson(request, 400, "{\"error\":\"Missing path\"}");
}

// ---- HID ----
static void handleHidType(AsyncWebServerRequest *request) {
  if (request->hasParam("text", true)) {
    String txt = request->getParam("text", true)->value();
    if (!usbHidIsReady()) usbHidStart();
    hidTypeString(txt);
    sendJson(request, 200, "{\"status\":\"typed\"}");
  } else sendJson(request, 400, "{\"error\":\"Missing text\"}");
}

static void handleHidScript(AsyncWebServerRequest *request) {
  if (request->hasParam("path", true)) {
    String p = request->getParam("path", true)->value();
    bool ok = hidRunScript(p);
    sendJson(request, ok ? 200 : 500, ok ? "{\"status\":\"started\"}" : "{\"error\":\"script failed\"}");
  } else sendJson(request, 400, "{\"error\":\"Missing path\"}");
}

// ---- Settings ----
static void handleSettings(AsyncWebServerRequest *request) {
  StaticJsonDocument<256> doc;
  doc["brightness"] = displayGetBacklight();
  doc["debounce"] = buttonsGetDebounceMs();
  doc["theme"] = displayGetThemeName();
  doc["mode"] = displayGetModeName();
  doc["orientation"] = displayGetRotation();
  doc["timeout"] = displayGetBacklightTimeout();
  String json; serializeJson(doc, json); sendJson(request, 200, json);
}

static void handleSettingsSet(AsyncWebServerRequest *request) {
  if (request->hasParam("brightness", true)) {
    int v = request->getParam("brightness", true)->value().toInt();
    displaySetBacklight(v);
  }
  if (request->hasParam("debounce", true)) {
    int v = request->getParam("debounce", true)->value().toInt();
    buttonsSetDebounceMs(v);
  }
  if (request->hasParam("orientation", true)) {
    int v = request->getParam("orientation", true)->value().toInt();
    displaySetRotation(v);
  }
  if (request->hasParam("timeout", true)) {
    int v = request->getParam("timeout", true)->value().toInt();
    displaySetBacklightTimeout(v);
  }
  sendJson(request, 200, "{\"status\":\"updated\"}");
}

// ========================================================================
//  STATIC FILE SERVER (from SD /web/)
// ========================================================================
static void handleStaticFile(AsyncWebServerRequest *request, const String& path) {
  String fullPath = "/web" + path;
  if (fullPath.endsWith("/")) fullPath += "index.html";
  String contentType = "text/html";
  if (fullPath.endsWith(".css")) contentType = "text/css";
  else if (fullPath.endsWith(".js")) contentType = "application/javascript";
  else if (fullPath.endsWith(".png")) contentType = "image/png";
  else if (fullPath.endsWith(".svg")) contentType = "image/svg+xml";
  else if (fullPath.endsWith(".json")) contentType = "application/json";
  if (!sdIsMounted() || !SD.exists(fullPath)) {
    request->send(404, "text/plain", "Not found");
    return;
  }
  AsyncWebServerResponse *response = request->beginResponse(SD, fullPath, contentType);
  request->send(response);
}

// ========================================================================
//  PUBLIC FUNCTIONS
// ========================================================================
void webStartServer() {
  if (serverStarted) return;

  // Ensure WiFi is in AP mode if not connected
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("GreyHat-Web", nullptr);
  }

  // ---- Register all routes ----
  server.on("/api/info", HTTP_GET, handleInfo);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/wifi/meter", HTTP_GET, handleWifiMeter);
  server.on("/api/wifi/deauth/start", HTTP_POST, handleDeauthStart);
  server.on("/api/wifi/deauth/stop", HTTP_POST, handleDeauthStop);
  server.on("/api/wifi/deauth/status", HTTP_GET, handleDeauthStatus);
  server.on("/api/wifi/beacon/start", HTTP_POST, handleBeaconStart);
  server.on("/api/wifi/beacon/stop", HTTP_POST, handleBeaconStop);
  server.on("/api/wifi/beacon/status", HTTP_GET, handleBeaconStatus);
  server.on("/api/wifi/packetmonitor", HTTP_GET, handlePacketMonitorStatus);
  server.on("/api/wifi/packetmonitor/reset", HTTP_POST, handlePacketMonitorReset);
  server.on("/api/wifi/channelanalyzer", HTTP_GET, handleChannelAnalyzer);
  server.on("/api/wifi/sniffer", HTTP_GET, handleSnifferStatus);
  server.on("/api/wifi/connect/list", HTTP_GET, handleConnectList);
  server.on("/api/wifi/connect", HTTP_POST, handleConnect);
  server.on("/api/wifi/scanhosts", HTTP_GET, handleScanHosts);
  server.on("/api/wifi/evilportal/status", HTTP_GET, handleEvilPortalStatus);
  server.on("/api/wifi/evilportal/start", HTTP_POST, handleEvilPortalStart);
  server.on("/api/wifi/evilportal/stop", HTTP_POST, handleEvilPortalStop);

  server.on("/api/ble/scan", HTTP_GET, handleBleScan);
  server.on("/api/ble/airtag", HTTP_GET, handleBleAirTag);
  server.on("/api/ble/skimmer", HTTP_GET, handleBleSkimmer);

  server.on("/api/ir/capture", HTTP_GET, handleIrCapture);
  server.on("/api/ir/send", HTTP_POST, handleIrSend);
  server.on("/api/ir/tvbgone/start", HTTP_POST, handleIrTvBGoneStart);
  server.on("/api/ir/tvbgone/stop", HTTP_POST, handleIrTvBGoneStop);
  server.on("/api/ir/jammer/start", HTTP_POST, handleIrJammerStart);
  server.on("/api/ir/jammer/stop", HTTP_POST, handleIrJammerStop);
  server.on("/api/ir/jammer/status", HTTP_GET, handleIrJammerStatus);
  server.on("/api/ir/universal/send", HTTP_POST, handleIrUniversalSend);

  server.on("/api/subghz/read", HTTP_GET, handleSubGhzRead);
  server.on("/api/subghz/send", HTTP_POST, handleSubGhzSend);
  server.on("/api/subghz/jammer/start", HTTP_POST, handleSubGhzJammerStart);
  server.on("/api/subghz/jammer/stop", HTTP_POST, handleSubGhzJammerStop);
  server.on("/api/subghz/jammer/status", HTTP_GET, handleSubGhzJammerStatus);
  server.on("/api/subghz/analyzer", HTTP_GET, handleSubGhzAnalyzer);

  server.on("/api/gps/fix", HTTP_GET, handleGpsFix);
  server.on("/api/gps/log/status", HTTP_GET, handleGpsLogStatus);
  server.on("/api/gps/log/start", HTTP_POST, handleGpsLogStart);
  server.on("/api/gps/log/stop", HTTP_POST, handleGpsLogStop);
  server.on("/api/gps/waypoints", HTTP_GET, handleGpsWaypoints);
  server.on("/api/gps/waypoint/add", HTTP_POST, handleGpsWaypointAdd);
  server.on("/api/gps/waypoint/delete", HTTP_POST, handleGpsWaypointDelete);
  server.on("/api/gps/wardrive/status", HTTP_GET, handleGpsWardriveStatus);
  server.on("/api/gps/wardrive/start", HTTP_POST, handleGpsWardriveStart);
  server.on("/api/gps/wardrive/stop", HTTP_POST, handleGpsWardriveStop);

  server.on("/api/lora/send", HTTP_POST, handleLoraSend);
  server.on("/api/lora/settings", HTTP_GET, handleLoraSettings);
  server.on("/api/lora/set", HTTP_POST, handleLoraSet);

  server.on("/api/wireguard/status", HTTP_GET, handleWgStatus);
  server.on("/api/wireguard/start", HTTP_POST, handleWgStart);
  server.on("/api/wireguard/stop", HTTP_POST, handleWgStop);

  server.on("/api/gpio/dio", HTTP_GET, handleGpioDio);
  server.on("/api/gpio/dio/set", HTTP_POST, handleGpioDioSet);
  server.on("/api/gpio/expander", HTTP_GET, handleGpioExpander);
  server.on("/api/gpio/expander/set", HTTP_POST, handleGpioExpanderSet);
  server.on("/api/i2c/scan", HTTP_GET, handleI2CScan);
  server.on("/api/spi/probe", HTTP_GET, handleSPIProbe);
  server.on("/api/uart/send", HTTP_POST, handleUartSend);
  server.on("/api/nfc/read", HTTP_GET, handleNfcRead);
  server.on("/api/onewire/read", HTTP_GET, handleOneWireRead);

  server.on("/api/files/list", HTTP_GET, handleFileList);
  server.on("/api/files/delete", HTTP_POST, handleFileDelete);
  server.on("/api/files/view", HTTP_GET, handleFileView);

  server.on("/api/hid/type", HTTP_POST, handleHidType);
  server.on("/api/hid/script", HTTP_POST, handleHidScript);

  server.on("/api/settings", HTTP_GET, handleSettings);
  server.on("/api/settings/set", HTTP_POST, handleSettingsSet);

  // ----- ROOT: try SD first, fallback to embedded HTML -----
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (sdIsMounted() && SD.exists("/web/index.html")) {
      AsyncWebServerResponse *response = request->beginResponse(SD, "/web/index.html", "text/html");
      response->addHeader("Cache-Control", "no-store");
      request->send(response);
      return;
    }
    // Fallback to flash
    AsyncWebServerResponse *response = request->beginResponse(200, "text/html", FALLBACK_HTML);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });

  // ---- Static files under /web/ ----
  server.onNotFound([](AsyncWebServerRequest *request) {
    String path = request->url();
    if (path.startsWith("/")) {
      handleStaticFile(request, path);
    } else {
      request->send(404);
    }
  });

  server.begin();
  serverStarted = true;

  // ================================================================
  //  TFT DISPLAY – enhanced with SD status and HTML file presence
  // ================================================================
  tft.fillScreen(displayColorBg());
  displayDrawHeaderBar("Web UI");

  tft.setTextColor(displayColorFg(), displayColorBg());
  tft.setTextSize(2);
  String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  tft.setCursor(10, displayHeaderHeight() + 20);
  tft.print("IP: " + ip);
  tft.setTextSize(1);

  // --- SD status ---
  tft.setCursor(10, displayHeaderHeight() + 50);
  tft.setTextColor(sdIsMounted() ? TFT_GREEN : TFT_RED, displayColorBg());
  tft.print("SD: ");
  tft.print(sdIsMounted() ? "Mounted" : "ERROR");

  // --- HTML file presence ---
  tft.setTextColor(displayColorFgDim(), displayColorBg());
  tft.setCursor(10, displayHeaderHeight() + 68);
  if (sdIsMounted()) {
    String content;
    bool hasHtml = sdReadFile("/web/index.html", content);
    tft.setTextColor(hasHtml ? TFT_GREEN : TFT_YELLOW, displayColorBg());
    tft.print("HTML: ");
    tft.print(hasHtml ? "Found" : "Missing");
  } else {
    tft.print("HTML: (SD unavailable)");
  }

  // --- Instructions ---
  tft.setTextColor(displayColorFgDim(), displayColorBg());
  tft.setCursor(10, displayHeaderHeight() + 90);
  tft.print("Open in browser");
  tft.setCursor(10, displayHeaderHeight() + 106);
  tft.print("http://" + ip);
}

void webStopServer() {
  if (serverStarted) {
    server.end();
    serverStarted = false;
  }
  if (WiFi.getMode() == WIFI_AP) {
    WiFi.softAPdisconnect(true);
  }
}

String webGetLocalIP() {
  return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
}

void webHandle() {
  // AsyncWebServer runs in background
}