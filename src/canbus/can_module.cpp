#include "can_module.h"
#include "config.h"
#include "display.h"
#include "buttons.h"
#include "menu.h"
#include "gpio_module.h"
#include "keyboard/keyboard_ui.h"
#include <SPI.h>
#include <ESP32-TWAI-CAN.hpp>
#include <mcp2515.h>

extern void enterState(int stateId);

// ============================================================
//  Configuration
// ============================================================
#define CAN_TWAI_TX_PIN   PIN_GPIO_IO1
#define CAN_TWAI_RX_PIN   PIN_GPIO_IO2
#define CAN_MCP_CS_PIN    PIN_GPIO_IO4

// Baud rates both drivers can hit
static const uint32_t CAN_BAUDS[] = {125000, 250000, 500000, 1000000};
static const char* CAN_BAUD_LABELS[] = {"125k", "250k", "500k", "1M"};
static const int CAN_BAUD_COUNT = 4;

// ============================================================
//  State
// ============================================================
enum CanPhase {
    CAN_PHASE_BUS_SELECT,
    CAN_PHASE_MODE_SELECT,
    CAN_PHASE_SNIFFER,
    CAN_PHASE_SEND_INPUT,
    CAN_PHASE_SEND_CONFIRM
};

enum CanBusType {
    CAN_BUS_TWAI,      // on-chip TWAI
    CAN_BUS_MCP2515    // external MCP2515
};

static CanPhase  canPhase = CAN_PHASE_BUS_SELECT;
static CanBusType canBus = CAN_BUS_TWAI;
static int       canBaudIndex = 2;   // 500 kbps default
static bool      canReady = false;
static uint8_t   canBaudStatusRetries = 0;

static SimpleMenu canBusMenu({"On-chip TWAI (ext. transceiver)", "MCP2515 (SPI)"});
static SimpleMenu canModeMenu({"Sniffer", "OBD-II Query", "Send Frame", "Set Baud"});

// Sniffer state
static uint32_t canFramesSeen = 0;
static uint32_t canLastFrameId = 0;
static uint8_t  canLastFrameData[8] = {0};
static uint8_t  canLastFrameDlc = 0;
static uint32_t canLastFrameCount = 0;
static unsigned long canLastDraw = 0;
static uint32_t canIdCount[32] = {0};   // 32 most recent IDs (rolling)
static int      canIdCountIdx = 0;

// OBD-II state
static int      obdPidIndex = 0;
static const uint8_t OBD_PIDS[] = {0x05, 0x0C, 0x0D, 0x0F, 0x10, 0x11, 0x1F};
static const char* OBD_NAMES[] = {
    "Coolant temp", "RPM", "Speed", "Intake temp",
    "MAF rate", "Throttle", "Run time"
};
static const int OBD_PID_COUNT = 7;
static String   obdLastReply = "";

// MCP2515 driver
static MCP2515* mcp = nullptr;

// ============================================================
//  Initialization
// ============================================================
static bool canInitTWAI() {
    ESP32Can.setPins(CAN_TWAI_TX_PIN, CAN_TWAI_RX_PIN);
    ESP32Can.setRxQueueSize(10);
    ESP32Can.setTxQueueSize(10);

    // Speed is fixed at 500 kbps for now — OBD-II standard.
    // Call begin() to (re)start with the selected baud.
    if (!ESP32Can.begin(ESP32Can.convertSpeed(CAN_BAUDS[canBaudIndex] / 1000))) {
        Serial.println("[CAN] TWAI begin failed");
        return false;
    }
    Serial.println("[CAN] TWAI ready");
    return true;
}

static bool canInitMCP2515() {
    if (!mcp) mcp = new MCP2515(CAN_MCP_CS_PIN);
    mcp->reset();

    // 8 MHz crystal is the most common on the blue MCP2515 modules.
    // If your module has a 16 MHz crystal, change MCP_8MHZ → MCP_16MHZ.
    CAN_SPEED speed;
    switch (CAN_BAUDS[canBaudIndex]) {
        case 125000:  speed = CAN_125KBPS; break;
        case 250000:  speed = CAN_250KBPS; break;
        case 1000000: speed = CAN_1000KBPS; break;
        case 500000:
        default:      speed = CAN_500KBPS; break;
    }
    if (mcp->setBitrate(speed, MCP_8MHZ) != MCP2515::ERROR_OK) {
        Serial.println("[CAN] MCP2515 setBitrate failed");
        return false;
    }
    mcp->setNormalMode();
    Serial.println("[CAN] MCP2515 ready");
    return true;
}

static bool canInit() {
    if (canReady) return true;

    // Use the dedicated GPIO SPI bus for MCP2515, NEVER the display/SD bus
    // (which is managed by TFT_eSPI and sdcard.cpp).
    if (canBus == CAN_BUS_MCP2515) {
        gpioGetSPI().begin(PIN_GPIO_IO1, PIN_GPIO_IO3, PIN_GPIO_IO2, CAN_MCP_CS_PIN);
        canReady = canInitMCP2515();
    } else {
        canReady = canInitTWAI();
    }
    return canReady;
}

bool canIsInitialized() { return canReady; }

void canDeinit() {
    if (!canReady) return;
    if (canBus == CAN_BUS_TWAI) {
        ESP32Can.end();
    } else {
        if (mcp) {
            mcp->reset();       // leaves MCP2515 in config mode (idle)
            delete mcp;
            mcp = nullptr;
        }
        gpioGetSPI().end();
    }
    canReady = false;
    Serial.println("[CAN] Deinit");
}

// ============================================================
//  Sniffer — reads all frames, shows most recent
// ============================================================
static void drawCanSniffer() {
    tft.fillScreen(displayColorBg());
    displayDrawHeaderBar("CAN Sniffer");

    int y = displayHeaderHeight() + 4;
    tft.setTextSize(1);

    // Stats
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(8, y);
    tft.print("Bus: " + String(canBus == CAN_BUS_TWAI ? "TWAI" : "MCP2515") +
              "  " + String(CAN_BAUD_LABELS[canBaudIndex]));
    y += 12;
    tft.setCursor(8, y);
    tft.print("Frames seen: " + String(canFramesSeen));
    y += 14;

    tft.drawFastHLine(4, y, tft.width() - 8, displayColorFgDim());
    y += 4;

    // Last frame readout
    if (canLastFrameCount == 0) {
        tft.setTextColor(displayColorFgDim(), displayColorBg());
        tft.setCursor(8, y + 20);
        tft.print("Waiting for frames...");
    } else {
        tft.setTextColor(TFT_GREEN, displayColorBg());
        tft.setCursor(8, y);
        tft.print("Last frame ID: 0x" + String(canLastFrameId, HEX));
        y += 12;

        tft.setTextColor(displayColorFg(), displayColorBg());
        tft.setCursor(8, y);
        tft.print("DLC " + String(canLastFrameDlc) + ":");
        y += 12;

        // Hex dump
        String hex;
        for (uint8_t i = 0; i < canLastFrameDlc; i++) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X ", canLastFrameData[i]);
            hex += buf;
        }
        tft.setCursor(8, y);
        tft.print(hex);
    }

    displayDrawFooterBar("OK pause  BACK exit");
}

static void canSnifferTick() {
    CanFrame rx;
    bool got = false;

    if (canBus == CAN_BUS_TWAI) {
        got = ESP32Can.readFrame(rx, 5);   // 5 ms timeout
        if (got) {
            canLastFrameId    = rx.identifier;
            canLastFrameDlc   = rx.data_length_code;
            memcpy(canLastFrameData, rx.data, rx.data_length_code);
        }
    } else {
        struct can_frame mrx;
        if (mcp->readMessage(&mrx) == MCP2515::ERROR_OK) {
            got = true;
            canLastFrameId  = mrx.can_id;
            canLastFrameDlc = mrx.can_dlc;
            memcpy(canLastFrameData, mrx.data, mrx.can_dlc);
        }
    }

    if (got) {
        canFramesSeen++;
        canLastFrameCount = millis();

        // Rolling ID histogram (used later for a top-IDs view)
        canIdCount[canIdCountIdx++ % 32] = canLastFrameId;
    }

    // Redraw at most 5 Hz so we don't hammer the SPI bus
    if (millis() - canLastDraw > 200) {
        canLastDraw = millis();
        drawCanSniffer();
    }
}

// ============================================================
//  OBD-II Query
// ============================================================
static void sendObdRequest(uint8_t pid) {
    if (canBus == CAN_BUS_TWAI) {
        CanFrame obd = {0};
        obd.identifier = 0x7DF;             // broadcast OBD2 address
        obd.extd = 0;
        obd.data_length_code = 8;
        obd.data[0] = 2;                    // length of service data
        obd.data[1] = 1;                    // service 01 (show current data)
        obd.data[2] = pid;
        for (int i = 3; i < 8; i++) obd.data[i] = 0xAA;  // pad, avoids bit-stuffing
        ESP32Can.writeFrame(obd, 50);
    } else {
        struct can_frame frame;
        frame.can_id = 0x7DF;
        frame.can_dlc = 8;
        frame.data[0] = 2;
        frame.data[1] = 1;
        frame.data[2] = pid;
        for (int i = 3; i < 8; i++) frame.data[i] = 0xAA;
        mcp->sendMessage(&frame);
    }
}

static String pollObdReply(uint32_t timeoutMs) {
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        if (canBus == CAN_BUS_TWAI) {
            CanFrame rx;
            if (ESP32Can.readFrame(rx, 20)) {
                if (rx.identifier == 0x7E8) {   // standard ECU response ID
                    char buf[64];
                    snprintf(buf, sizeof(buf),
                             "Reply: %02X %02X %02X %02X %02X %02X %02X %02X",
                             rx.data[0], rx.data[1], rx.data[2], rx.data[3],
                             rx.data[4], rx.data[5], rx.data[6], rx.data[7]);
                    return String(buf);
                }
            }
        } else {
            struct can_frame rx;
            if (mcp->readMessage(&rx) == MCP2515::ERROR_OK) {
                if (rx.can_id == 0x7E8) {
                    char buf[64];
                    snprintf(buf, sizeof(buf),
                             "Reply: %02X %02X %02X %02X %02X %02X %02X %02X",
                             rx.data[0], rx.data[1], rx.data[2], rx.data[3],
                             rx.data[4], rx.data[5], rx.data[6], rx.data[7]);
                    return String(buf);
                }
            }
        }
    }
    return "(no reply)";
}

static void runObdQuery(int pidIndex) {
    if (!canReady) {
        displayShowMessage("CAN", "Bus not initialized");
        delay(1000);
        return;
    }

    // Draw query screen
    tft.fillScreen(displayColorBg());
    displayDrawHeaderBar("OBD-II Query");
    tft.setTextSize(2);
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(10, displayHeaderHeight() + 20);
    tft.print(OBD_NAMES[pidIndex]);
    tft.setTextSize(1);
    tft.setCursor(10, displayHeaderHeight() + 50);
    tft.print("PID: 0x" + String(OBD_PIDS[pidIndex], HEX));
    tft.setCursor(10, displayHeaderHeight() + 80);
    tft.print("Sending request...");

    sendObdRequest(OBD_PIDS[pidIndex]);
    String reply = pollObdReply(500);

    tft.setCursor(10, displayHeaderHeight() + 100);
    tft.print(reply);

    // Decode into human value for common PIDs
    tft.setTextColor(TFT_GREEN, displayColorBg());
    tft.setCursor(10, displayHeaderHeight() + 140);
    // (Decoding requires more parsing — display the raw hex for now)

    displayDrawFooterBar("UP/DN pid  OK resend  BACK exit");
    obdLastReply = reply;
}

static void drawCanObd() {
    if (obdLastReply.length() == 0) {
        // Fresh entry — run first query
        runObdQuery(obdPidIndex);
    } else {
        tft.fillScreen(displayColorBg());
        displayDrawHeaderBar("OBD-II Query");
        tft.setTextSize(2);
        tft.setTextColor(displayColorFg(), displayColorBg());
        tft.setCursor(10, displayHeaderHeight() + 20);
        tft.print(OBD_NAMES[obdPidIndex]);
        tft.setTextSize(1);
        tft.setCursor(10, displayHeaderHeight() + 50);
        tft.print("PID: 0x" + String(OBD_PIDS[obdPidIndex], HEX));
        tft.setCursor(10, displayHeaderHeight() + 100);
        tft.print(obdLastReply);
        displayDrawFooterBar("UP/DN pid  OK resend  BACK exit");
    }
}

// ============================================================
//  Send Frame
// ============================================================
static void canSendFrameFromKeyboard() {
    OnScreenKeyboardConfig cfg;
    osKeyboardUseStandardLayout(cfg);
    cfg.title = "CAN Send";
    cfg.subtitle = "Frame ID (hex)";
    cfg.maxLen = 8;
    cfg.okLabel = "Next";
    cfg.requireNonEmpty = true;
    String idStr;
    osKeyboardEnter(cfg, idStr);
    if (osKeyboardWasCancelled()) return;
    uint32_t id = strtoul(idStr.c_str(), nullptr, 16);

    cfg.title = "CAN Send";
    cfg.subtitle = "DLC (0-8)";
    cfg.maxLen = 1;
    cfg.okLabel = "Next";
    cfg.requireNonEmpty = true;
    String dlcStr;
    osKeyboardEnter(cfg, dlcStr);
    if (osKeyboardWasCancelled()) return;
    uint8_t dlc = strtoul(dlcStr.c_str(), nullptr, 10);
    if (dlc > 8) dlc = 8;

    cfg.title = "CAN Send";
    cfg.subtitle = "Data bytes (hex, space-sep)";
    cfg.maxLen = 24;
    cfg.okLabel = "Send";
    cfg.requireNonEmpty = true;
    String dataStr;
    osKeyboardEnter(cfg, dataStr);
    if (osKeyboardWasCancelled()) return;

    // Parse hex bytes
    uint8_t data[8] = {0};
    int i = 0, idx = 0;
    while (i < (int)dataStr.length() && idx < dlc) {
        while (i < (int)dataStr.length() && dataStr[i] == ' ') i++;
        if (i >= (int)dataStr.length()) break;
        int start = i;
        while (i < (int)dataStr.length() && dataStr[i] != ' ') i++;
        String tok = dataStr.substring(start, i);
        data[idx++] = (uint8_t)strtol(tok.c_str(), nullptr, 16);
    }

    // Send
    bool ok = false;
    if (canBus == CAN_BUS_TWAI) {
        CanFrame tx = {0};
        tx.identifier = id;
        tx.extd = (id > 0x7FF) ? 1 : 0;
        tx.data_length_code = dlc;
        memcpy(tx.data, data, dlc);
        ok = ESP32Can.writeFrame(tx, 100);
    } else {
        struct can_frame tx;
        tx.can_id = id;
        tx.can_dlc = dlc;
        memcpy(tx.data, data, dlc);
        ok = (mcp->sendMessage(&tx) == MCP2515::ERROR_OK);
    }

    displayShowMessage("CAN Send", ok ? "Frame sent." : "Send failed.");
    delay(1000);
}

// ============================================================
//  Menu + dispatch
// ============================================================
void gpioEnterCAN() {
    canPhase = CAN_PHASE_BUS_SELECT;
    displayShowMenu("GPIO - CAN Bus", canBusMenu.items(), canBusMenu.index());
}

static void canEnterModeMenu() {
    canPhase = CAN_PHASE_MODE_SELECT;
    displayShowMenu("CAN - Mode", canModeMenu.items(), canModeMenu.index());
}

void gpioHandleCANEvent(int evt) {
    if (evt == EVT_NONE) return;

    // ---- Bus selection ----
    if (canPhase == CAN_PHASE_BUS_SELECT) {
        if (evt == EVT_UP)   { canBusMenu.up();   gpioEnterCAN(); return; }
        if (evt == EVT_DOWN) { canBusMenu.down(); gpioEnterCAN(); return; }
        if (evt == EVT_OK) {
            canBus = (canBusMenu.index() == 0) ? CAN_BUS_TWAI : CAN_BUS_MCP2515;
            canDeinit();   // tear down whatever was up
            if (!canInit()) {
                displayShowMessage("CAN", "Init failed.\nCheck wiring & power.");
                delay(2000);
                gpioEnterCAN();
                return;
            }
            canEnterModeMenu();
        }
        return;
    }

    // ---- Mode selection ----
    if (canPhase == CAN_PHASE_MODE_SELECT) {
        if (evt == EVT_UP)   { canModeMenu.up();   canEnterModeMenu(); return; }
        if (evt == EVT_DOWN) { canModeMenu.down(); canEnterModeMenu(); return; }
        if (evt == EVT_OK) {
            switch (canModeMenu.index()) {
                case 0:
                    canPhase = CAN_PHASE_SNIFFER;
                    canFramesSeen = 0;
                    canLastFrameCount = 0;
                    drawCanSniffer();
                    break;
                case 1:
                    obdPidIndex = 0;
                    obdLastReply = "";
                    drawCanObd();
                    break;
                case 2:
                    canSendFrameFromKeyboard();
                    canEnterModeMenu();
                    break;
                case 3:
                    // Cycle baud rate and re-init
                    canBaudIndex = (canBaudIndex + 1) % CAN_BAUD_COUNT;
                    canDeinit();
                    canInit();
                    canEnterModeMenu();
                    break;
            }
            return;
        }
        if (evt == EVT_BACK) {
            canDeinit();
            gpioEnterCAN();
        }
        return;
    }

    // ---- Sniffer ----
    if (canPhase == CAN_PHASE_SNIFFER) {
        if (evt == EVT_BACK) {
            canEnterModeMenu();
            return;
        }
        canSnifferTick();
        return;
    }

    // ---- OBD-II ----
    if (canPhase == CAN_PHASE_MODE_SELECT || evt == EVT_BACK) {
        // handled above
    }
    // OBD-II is handled implicitly via the mode menu; back is generic.
    if (evt == EVT_BACK) {
        canEnterModeMenu();
    }
}