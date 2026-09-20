#include "nrf_module.h"
#include "config.h"
#include "display.h"
#include "buttons.h"
#include "menu.h"
#include "gpio_module.h"
#include <SPI.h>
#include <RF24.h>

extern void enterState(int stateId);

// ---- Pin mapping on the GPIO header ----
#define NRF_CE_PIN    PIN_GPIO_IO4
#define NRF_CSN_PIN   PIN_GPIO_IO5

// ---- Radio instance (uses the dedicated GPIO SPI bus) ----
static RF24 radio(NRF_CE_PIN, NRF_CSN_PIN);
static bool nrfInitialized = false;
static bool nrfChipFound = false;

// ---- Scanner state ----
static const uint8_t NRF_CHANNELS = 126;
static uint8_t  nrfChannelValues[NRF_CHANNELS];
static const int NRF_SCAN_PASSES = 100;
static const uint8_t noiseAddress[][2] = {
    {0x55, 0x55}, {0xAA, 0xAA}, {0xA0, 0xAA},
    {0xAB, 0xAA}, {0xAC, 0xAA}, {0xAD, 0xAA}
};
static const int noiseAddressCount = 6;

// ---- Sniffer state ----
static String   nrfLastPacket = "";
static uint8_t  nrfLastPacketRaw[32];
static uint8_t  nrfLastPacketLen = 0;
static int8_t   nrfLastPacketChannel = -1;
static uint32_t nrfPacketsSeen = 0;
static unsigned long nrfLastSniffDraw = 0;

// ---- Mode state ----
enum NrfMode { NRF_MODE_MENU, NRF_MODE_SCANNER, NRF_MODE_SNIFFER };
static NrfMode nrfMode = NRF_MODE_MENU;
static SimpleMenu nrfMenu({"Scanner", "Sniffer", "Channel Analyzer"});
static int nrfScannerChannel = 0;

// ============================================================
//  Initialization
// ============================================================
static bool nrfInit() {
    if (nrfInitialized) return nrfChipFound;

    // Use the dedicated GPIO SPI bus — NEVER the display/SD SPI bus.
    if (!radio.begin(&gpioGetSPI())) {
        nrfChipFound = false;
        nrfInitialized = true;
        Serial.println("[NRF] radio.begin() failed");
        return false;
    }

    if (!radio.isChipConnected()) {
        nrfChipFound = false;
        nrfInitialized = true;
        Serial.println("[NRF] Chip not connected");
        return false;
    }

    // Conservative defaults — high power on a bare module causes brownouts.
    radio.setPALevel(RF24_PA_LOW);
    radio.setDataRate(RF24_1MBPS);
    radio.setAutoAck(false);
    radio.setCRCLength(RF24_CRC_DISABLED);
    radio.setAddressWidth(2);
    radio.setPayloadSize(32);
    radio.stopListening();

    nrfChipFound = true;
    nrfInitialized = true;
    Serial.println("[NRF] Init OK");
    return true;
}

bool nrfIsInitialized() { return nrfInitialized; }

void nrfDeinit() {
    if (!nrfInitialized) return;
    radio.stopListening();
    radio.powerDown();
    nrfInitialized = false;
    nrfChipFound = false;
    Serial.println("[NRF] Deinit OK");
}

// ============================================================
//  Scanner — sweeps all 126 channels, counts signal hits
// ============================================================
static void drawNrfScanner() {
    tft.fillScreen(displayColorBg());
    displayDrawHeaderBar("NRF24 Scanner");

    const int headerH = displayHeaderHeight();
    int graphTop = headerH + 8;
    int graphH   = tft.height() - graphTop - 40;
    int graphX   = 8;
    int graphW   = tft.width() - 16;

    tft.setTextSize(1);
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(8, graphTop);
    tft.print("2.4GHz spectrum (126 ch)");
    graphTop += 14;

    // Find the peak value for auto-scaling
    uint8_t peak = 1;
    for (int i = 0; i < NRF_CHANNELS; i++) {
        if (nrfChannelValues[i] > peak) peak = nrfChannelValues[i];
    }

    // Background grid
    tft.drawRect(graphX, graphTop, graphW, graphH, displayColorFgDim());
    for (int y = 1; y < 4; y++) {
        int gy = graphTop + (graphH * y) / 4;
        tft.drawFastHLine(graphX + 1, gy, graphW - 2, displayColorFgDim());
    }

    // Bar chart: pack 126 channels into graphW pixels (each ~1px)
    int barW = max(1, graphW / NRF_CHANNELS);
    for (int i = 0; i < NRF_CHANNELS; i++) {
        int x = graphX + 1 + (i * (graphW - 2)) / NRF_CHANNELS;
        int h = (nrfChannelValues[i] * (graphH - 4)) / peak;
        if (h < 1 && nrfChannelValues[i] > 0) h = 1;

        uint16_t color;
        if (nrfChannelValues[i] == 0)     color = displayColorFgDim();
        else if (nrfChannelValues[i] > peak / 2) color = TFT_RED;
        else if (nrfChannelValues[i] > peak / 4) color = TFT_YELLOW;
        else                                     color = TFT_GREEN;

        if (h > 0) tft.fillRect(x, graphTop + graphH - h, barW, h, color);
    }

    // Channel axis labels
    tft.setTextColor(displayColorFgDim(), displayColorBg());
    tft.setCursor(8, graphTop + graphH + 4);
    tft.print("0");
    tft.setCursor(tft.width() / 2 - 10, graphTop + graphH + 4);
    tft.print("62");
    tft.setCursor(tft.width() - 24, graphTop + graphH + 4);
    tft.print("125");

    // Peak channel readout
    int peakCh = 0;
    for (int i = 1; i < NRF_CHANNELS; i++) {
        if (nrfChannelValues[i] > nrfChannelValues[peakCh]) peakCh = i;
    }
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(8, tft.height() - 24);
    tft.print("Peak: ch" + String(peakCh) + " (" +
              String((2400 + peakCh) ) + " MHz, " +
              String(nrfChannelValues[peakCh]) + " hits)");

    displayDrawFooterBar("OK rescan  BACK exit");
}

static void nrfRunScan() {
    memset(nrfChannelValues, 0, sizeof(nrfChannelValues));

    // Prime radio with worst-case addresses to catch background noise.
    // (See RF24 scanner example — this is the standard technique.)
    radio.stopListening();
    radio.setAutoAck(false);
    radio.setCRCLength(RF24_CRC_DISABLED);

    // Start scan with progress bar
    tft.fillScreen(displayColorBg());
    displayDrawHeaderBar("NRF24 Scanner");
    tft.setTextSize(2);
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(10, displayHeaderHeight() + 40);
    tft.print("Scanning...");
    tft.setTextSize(1);

    for (int rep = 0; rep < NRF_SCAN_PASSES; rep++) {
        for (int i = 0; i < NRF_CHANNELS; i++) {
            radio.setChannel(i);
            radio.startListening();
            delayMicroseconds(128);   // ~128us dwell per channel
            bool found = radio.testRPD();
            if (found || radio.available()) {
                if (nrfChannelValues[i] < 255) nrfChannelValues[i]++;
                radio.flush_rx();
            }
            radio.stopListening();
        }

        // Progress bar — update every 10 passes
        if (rep % 10 == 0) {
            int pct = (rep * 100) / NRF_SCAN_PASSES;
            int barY = tft.height() - 60;
            tft.drawRect(10, barY, tft.width() - 20, 12, displayColorFgDim());
            tft.fillRect(12, barY + 2,
                         ((tft.width() - 24) * pct) / 100, 8,
                         TFT_GREEN);
        }
    }

    Serial.println("[NRF] Scan complete");
}

// ============================================================
//  Sniffer — hops channels, shows raw packets received
// ============================================================
static void drawNrfSniffer() {
    tft.fillScreen(displayColorBg());
    displayDrawHeaderBar("NRF24 Sniffer");

    int y = displayHeaderHeight() + 6;
    tft.setTextSize(1);

    // Status
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(8, y);
    tft.print("Channel: ch" + String(radio.getChannel()));
    y += 12;
    tft.setCursor(8, y);
    tft.print("Packets seen: " + String(nrfPacketsSeen));
    y += 16;

    // Divider
    tft.drawFastHLine(4, y, tft.width() - 8, displayColorFgDim());
    y += 4;

    if (nrfLastPacketLen == 0) {
        tft.setTextColor(displayColorFgDim(), displayColorBg());
        tft.setCursor(8, y + 20);
        tft.print("Waiting for packets...");
        displayDrawFooterBar("UP/DN ch  BACK exit");
        return;
    }

    // Last packet header
    tft.setTextColor(TFT_GREEN, displayColorBg());
    tft.setCursor(8, y);
    tft.print("Last packet (ch" + String(nrfLastPacketChannel) +
              ", " + String(nrfLastPacketLen) + " bytes)");
    y += 14;

    // Hex dump — 8 bytes per row
    tft.setTextColor(displayColorFg(), displayColorBg());
    for (uint8_t i = 0; i < nrfLastPacketLen && y < tft.height() - 20; i++) {
        if (i % 8 == 0) {
            tft.setCursor(8, y);
            char addr[6];
            snprintf(addr, sizeof(addr), "%02X:", i);
            tft.setTextColor(displayColorFgDim(), displayColorBg());
            tft.print(addr);
            tft.setTextColor(displayColorFg(), displayColorBg());
            y += 12;
        }
        char hex[4];
        snprintf(hex, sizeof(hex), "%02X ", nrfLastPacketRaw[i]);
        tft.setCursor(8 + (i % 8) * 28, y - 12);
        tft.print(hex);
    }

    displayDrawFooterBar("UP/DN ch  BACK exit");
}

static void nrfSnifferTick() {
    // Hop through channels 0-125 in a round-robin
    static int sniffChannel = 0;
    radio.setChannel(sniffChannel);
    sniffChannel = (sniffChannel + 1) % NRF_CHANNELS;

    radio.startListening();
    delay(1);

    if (radio.available()) {
        uint8_t buf[32] = {0};
        uint8_t len = radio.getPayloadSize();
        radio.read(buf, len);

        memcpy(nrfLastPacketRaw, buf, len);
        nrfLastPacketLen = len;
        nrfLastPacketChannel = sniffChannel;
        nrfPacketsSeen++;

        // Render ASCII representation
        nrfLastPacket = "";
        for (uint8_t i = 0; i < len; i++) {
            char c = buf[i];
            if (c >= 32 && c < 127) nrfLastPacket += c;
            else                    nrfLastPacket += '.';
        }
        radio.flush_rx();
    }

    radio.stopListening();
}

// ============================================================
//  Public UI entry points
// ============================================================
void gpioEnterNRF() {
    nrfMode = NRF_MODE_MENU;
    nrfInit();
    displayShowMenu("GPIO - nRF24", nrfMenu.items(), nrfMenu.index());
}

void gpioHandleNRFEvent(int evt) {
    if (evt == EVT_NONE) return;

    if (nrfMode == NRF_MODE_MENU) {
        if (evt == EVT_UP)   { nrfMenu.up();   gpioEnterNRF(); return; }
        if (evt == EVT_DOWN) { nrfMenu.down(); gpioEnterNRF(); return; }
        if (evt == EVT_OK) {
            if (!nrfChipFound) {
                displayShowMessage("nRF24", "Chip not found.\nCheck wiring:\n"
                    "IO1=SCK IO2=MOSI\nIO3=MISO\nIO4=CE IO5=CSN\n"
                    "3.3V + capacitor!");
                delay(3000);
                return;
            }
            switch (nrfMenu.index()) {
                case 0:
                    nrfMode = NRF_MODE_SCANNER;
                    nrfRunScan();
                    drawNrfScanner();
                    break;
                case 1:
                    nrfMode = NRF_MODE_SNIFFER;
                    nrfPacketsSeen = 0;
                    nrfLastPacketLen = 0;
                    drawNrfSniffer();
                    break;
                case 2:
                    // Channel Analyzer uses the same routine as Scanner
                    nrfMode = NRF_MODE_SCANNER;
                    nrfRunScan();
                    drawNrfScanner();
                    break;
            }
            return;
        }
        return;
    }

    if (nrfMode == NRF_MODE_SCANNER) {
        if (evt == EVT_OK) {
            nrfRunScan();
            drawNrfScanner();
        } else if (evt == EVT_BACK) {
            nrfMode = NRF_MODE_MENU;
            gpioEnterNRF();
        }
        return;
    }

    if (nrfMode == NRF_MODE_SNIFFER) {
        if (evt == EVT_BACK) {
            nrfMode = NRF_MODE_MENU;
            gpioEnterNRF();
        }
        return;
    }
}