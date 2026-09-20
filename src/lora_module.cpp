#include "lora_module.h"
#include "config.h"
#include "display.h"
#include "buttons.h"
#include "menu.h"
#include "helpers.h"
#include <SPI.h>
#include <LoRa.h>

// ====================================================================
//  Core LoRa (unchanged)
// ====================================================================
static bool ready = false;
static LoraBand band = LoraBand::MHz433;
static int spreadingFactor = 9;
static int txPowerDbm = 14;

static uint32_t rxCount = 0;
static uint32_t txCount = 0;
static int lastRssi = 0;
static float lastSnr = 0.0f;
static unsigned long lastRxMs = 0;

static long bandFrequencyHz(LoraBand b) {
  switch (b) {
    case LoraBand::MHz433: return 433E6;
    case LoraBand::MHz868: return 868E6;
    case LoraBand::MHz915: return 915E6;
    default: return 433E6;
  }
}

static void applySettings() {
  if (!ready) return;
  LoRa.setSpreadingFactor(spreadingFactor);
  LoRa.setTxPower(txPowerDbm);
}

bool loraInit() {
  LoRa.setPins(PIN_LORA_CS, PIN_LORA_RST, PIN_LORA_DIO0);
  ready = LoRa.begin(bandFrequencyHz(band));
  if (ready) applySettings();
  return ready;
}

bool loraIsReady() { return ready; }

bool loraSend(const String& text) {
  if (!ready) return false;
  LoRa.beginPacket();
  LoRa.print(text);
  bool ok = (LoRa.endPacket() == 1);
  if (ok) txCount++;
  return ok;
}

bool loraPoll(String& outText, int& outRssi, float& outSnr) {
  if (!ready) return false;
  int packetSize = LoRa.parsePacket();
  if (packetSize == 0) return false;
  String text;
  while (LoRa.available()) text += (char)LoRa.read();
  lastRssi = LoRa.packetRssi();
  lastSnr = LoRa.packetSnr();
  lastRxMs = millis();
  rxCount++;
  outText = text;
  outRssi = lastRssi;
  outSnr = lastSnr;
  return true;
}

uint32_t loraGetRxCount() { return rxCount; }
uint32_t loraGetTxCount() { return txCount; }
int loraGetLastRssi() { return lastRssi; }
float loraGetLastSnr() { return lastSnr; }
unsigned long loraGetLastRxMs() { return lastRxMs; }

void loraSetBand(LoraBand b) {
  band = b;
  if (ready) {
    LoRa.end();
    ready = LoRa.begin(bandFrequencyHz(band));
    if (ready) applySettings();
  }
}
LoraBand loraGetBand() { return band; }
const char* loraGetBandName() {
  switch (band) {
    case LoraBand::MHz433: return "433 MHz";
    case LoraBand::MHz868: return "868 MHz";
    case LoraBand::MHz915: return "915 MHz";
    default: return "?";
  }
}

void loraSetSpreadingFactor(int sf) {
  if (sf < 6) sf = 6;
  if (sf > 12) sf = 12;
  spreadingFactor = sf;
  applySettings();
}
int loraGetSpreadingFactor() { return spreadingFactor; }

void loraSetTxPower(int dbm) {
  if (dbm < 2) dbm = 2;
  if (dbm > 20) dbm = 20;
  txPowerDbm = dbm;
  applySettings();
}
int loraGetTxPower() { return txPowerDbm; }

// ====================================================================
//  LoRa UI (moved from main.cpp)
// ====================================================================
static SimpleMenu loraMenu({"Chat / Beacon", "Packet Monitor", "Settings"});
static int loraPingCounter = 0;
static int loraSettingsRow = 0;
static unsigned long lastLoraMonitorRedraw = 0;

static void drawLoraMonitor() {
  tft.fillScreen(displayColorBg());
  displayDrawHeaderBar("LoRa Monitor");
  int y = displayHeaderHeight() + 10;
  tft.setTextSize(1);
  tft.setTextColor(displayColorFg(), displayColorBg());
  tft.setCursor(8, y); tft.print(loraIsReady() ? "Radio: OK" : "Radio: NOT DETECTED"); y += 14;
  tft.setCursor(8, y); tft.print(String("Band: ") + loraGetBandName()); y += 14;
  tft.setCursor(8, y); tft.print("SF: " + String(loraGetSpreadingFactor()) + "  Pwr: " + String(loraGetTxPower()) + "dBm"); y += 14;
  tft.setCursor(8, y); tft.print("RX: " + String(loraGetRxCount()) + "  TX: " + String(loraGetTxCount())); y += 14;
  tft.setCursor(8, y); tft.print("Last RSSI: " + String(loraGetLastRssi()) + " dBm"); y += 14;
  tft.setCursor(8, y); tft.print("Last SNR: " + String(loraGetLastSnr(), 1) + " dB"); y += 14;
  unsigned long lastRx = loraGetLastRxMs();
  tft.setCursor(8, y);
  tft.print(lastRx > 0 ? ("Last RX: " + String((millis() - lastRx) / 1000) + "s ago") : "Last RX: none yet");
  y += 20;
  tft.setTextColor(displayColorFgDim(), displayColorBg());
  tft.setCursor(8, y);
  tft.print("OK=send test ping");
}

static void drawLoraSettings() {
  tft.fillScreen(displayColorBg());
  displayDrawHeaderBar("LoRa Settings");
  int y = displayHeaderHeight() + 16;
  tft.setTextSize(2);
  const int rows = 3;
  for (int row = 0; row < rows; row++) {
    bool sel = (row == loraSettingsRow);
    tft.setTextColor(sel ? displayColorSelectFg() : displayColorFg(),
                      sel ? displayColorSelectBg() : displayColorBg());
    tft.setCursor(10, y);
    switch (row) {
      case 0: tft.print(">Band: " + String(loraGetBandName())); break;
      case 1: tft.print(">SF: " + String(loraGetSpreadingFactor())); break;
      case 2: tft.print(">Power: " + String(loraGetTxPower()) + "dBm"); break;
    }
    y += 30;
  }
  tft.setTextSize(1);
  tft.setTextColor(displayColorFgDim(), displayColorBg());
  tft.setCursor(10, y + 10);
  tft.print("UP/DOWN=select LEFT/RIGHT=adjust");
  if (!loraIsReady()) {
    tft.setCursor(10, y + 24);
    tft.print("WARNING: radio init failed");
  }
}

void loraEnterMenu() {
    // Lazy init on first entry. If the radio doesn't respond, we still
    // show the menu, but every operation inside will fail gracefully.
    if (!ready) {
        loraInit();   // returns bool, but we don't gate the menu on it
    }
    displayShowMenu("LoRa", loraMenu.items(), loraMenu.index());
}

void loraHandleMenuEvent(int evt) {
  if (evt == EVT_UP)   { loraMenu.up();   loraEnterMenu(); }
  if (evt == EVT_DOWN) { loraMenu.down(); loraEnterMenu(); }
  if (evt == EVT_OK) {
    switch (loraMenu.index()) {
      case 0: enterState(STATE_LORA_CHAT);     break;
      case 1: enterState(STATE_LORA_MONITOR);  break;
      case 2: enterState(STATE_LORA_SETTINGS); break;
    }
  }
}

void loraEnterChat() {
  displayShowMessage("LoRa Chat/Beacon", "Listening...\nOK = send ping\nBACK to exit");
}

void loraHandleChatEvent(int evt, bool gotPacket, const String& packet, int rssi, float snr) {
  if (gotPacket) {
    String msg = "RX RSSI:" + String(rssi) + "dBm SNR:" + String(snr, 1) + "dB\n" + packet;
    displayShowMessage("LoRa Chat/Beacon", msg.c_str());
  }
  if (evt == EVT_OK) {
    loraPingCounter++;
    String msg = "PING #" + String(loraPingCounter) + " from " + String(DEVICE_NAME);
    bool ok = loraSend(msg);
    displayShowMessage("LoRa Chat/Beacon", ok ? ("Sent: " + msg).c_str() : "Send failed (radio?)");
    delay(400);
  }
}

void loraEnterMonitor() {
  lastLoraMonitorRedraw = 0;
  drawLoraMonitor();
}

void loraHandleMonitorEvent(int evt, bool gotPacket) {
  if (evt == EVT_OK) {
    loraPingCounter++;
    loraSend("PING #" + String(loraPingCounter) + " from " + String(DEVICE_NAME));
  }
  unsigned long now = millis();
  if (gotPacket || now - lastLoraMonitorRedraw > 500) {
    lastLoraMonitorRedraw = now;
    drawLoraMonitor();
  }
}

void loraEnterSettings() {
  loraSettingsRow = 0;
  drawLoraSettings();
}

void loraHandleSettingsEvent(int evt) {
  if (evt == EVT_UP)   loraSettingsRow = (loraSettingsRow > 0) ? loraSettingsRow - 1 : 2;
  if (evt == EVT_DOWN) loraSettingsRow = (loraSettingsRow < 2) ? loraSettingsRow + 1 : 0;
  if (evt == EVT_LEFT || evt == EVT_RIGHT) {
    int dir = (evt == EVT_RIGHT) ? 1 : -1;
    switch (loraSettingsRow) {
      case 0: {
        int count = (int)LoraBand::Count;
        int b = (((int)loraGetBand() + dir) % count + count) % count;
        loraSetBand((LoraBand)b);
        break;
      }
      case 1:
        loraSetSpreadingFactor(loraGetSpreadingFactor() + dir);
        break;
      case 2:
        loraSetTxPower(loraGetTxPower() + dir * 2);
        break;
    }
  }
  if (evt != EVT_NONE) drawLoraSettings();
}

void loraDeinit() {
    if (ready) {
        LoRa.sleep();


        digitalWrite(PIN_LORA_CS, HIGH);

        ready = false;   // will re-init on next loraEnterMenu()
    }
}