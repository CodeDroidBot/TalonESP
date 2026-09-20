#include "pn532_nfc.h"
#include "pn532_common.h"
#include "config.h"
#include "display.h"
#include "buttons.h"
#include "menu.h"
#include "gpio_module.h"

// Emulation
#include <EmulateTag.h>

extern void enterState(int stateId);

// ============================================================
//  State
// ============================================================
enum NfcScreen { NFC_MENU, NFC_EMULATING };
static NfcScreen nfcScreen = NFC_MENU;

static SimpleMenu nfcMenu({"Read Tag Info", "Emulate Tag", "Firmware Info"});

static uint8_t nfcEmuUid[4] = {0x08, 0x11, 0x22, 0x33};

// EmulateTag takes a reference to the interface — construct it as a
// singleton on first use.
static EmulateTag* nfcEmu = nullptr;

// ============================================================
//  Drawing
// ============================================================
static void drawNfcMenu() {
    displayShowMenu("NFC", nfcMenu.items(), nfcMenu.index());
}

static void drawNfcMessage(const char* title, const String& msg) {
    tft.fillScreen(displayColorBg());
    displayDrawHeaderBar(title);
    tft.setTextSize(1);
    tft.setTextColor(displayColorFg(), displayColorBg());
    int y = displayHeaderHeight() + 8;
    int start = 0;
    while (start < (int)msg.length()) {
        int nl = msg.indexOf('\n', start);
        String line = (nl < 0) ? msg.substring(start) : msg.substring(start, nl);
        start = (nl < 0) ? msg.length() : nl + 1;
        if (y > tft.height() - 22) break;
        tft.setCursor(8, y);
        tft.print(line);
        y += 11;
    }
    displayDrawFooterBar("OK retry  BACK exit");
}

// ============================================================
//  Feature: read tag info
// ============================================================
static void nfcReadTagInfo() {
    tft.fillScreen(displayColorBg());
    displayDrawHeaderBar("NFC Read");
    tft.setTextSize(1);
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(8, displayHeaderHeight() + 10);
    tft.print("Waiting for tag...");
    tft.setCursor(8, displayHeaderHeight() + 30);
    tft.print("Hold an NFC tag near");
    tft.setCursor(8, displayHeaderHeight() + 42);
    tft.print("the antenna.");
    displayDrawFooterBar("BACK cancel");

    uint8_t uid[7] = {0};
    uint8_t uidLen = 0;

    uint32_t start = millis();
    bool gotTag = false;
    while (millis() - start < 8000) {
        int evt = buttonsPoll();
        if (evt == EVT_BACK) { drawNfcMenu(); return; }
        if (pn532ReadUid(uid, uidLen, 100)) { gotTag = true; break; }
        delay(30);
    }

    if (!gotTag) {
        drawNfcMessage("NFC Read", "No tag detected.");
        return;
    }

    String info = "UID:    " + pn532UidToString(uid, uidLen) + "\n";
    info += "Length: " + String(uidLen) + " bytes\n";

    if (uidLen == 4)      info += "Type:   Mifare Classic /\n        Ultralight / NTAG\n";
    else if (uidLen == 7) info += "Type:   Ultralight / NTAG\n";
    else                  info += "Type:   Unknown\n";

    // Try reading a Mifare Ultralight / NTAG page (page 3 = CC)
    uint8_t page3[4] = {0};
    if (nfc.mifareultralight_ReadPage(3, page3)) {
        info += "\nNTAG detected.\n";
        info += "CC: ";
        for (int i = 0; i < 4; i++) {
            if (page3[i] < 0x10) info += "0";
            info += String(page3[i], HEX);
            if (i < 3) info += " ";
        }
        info += "\n";
        if ((page3[0] & 0xF0) == 0xE0) info += "NDEF-formatted\n";
        else                            info += "Not NDEF-formatted\n";
    }

    drawNfcMessage("NFC Tag Info", info);
}

// ============================================================
//  Feature: emulate tag
// ============================================================
static void nfcStartEmulation() {
    if (!nfcEmu) nfcEmu = new EmulateTag(pn532_iface);

    // Configure the tag we'll present
    nfcEmu->setUid(nfcEmuUid);
    nfcEmu->init();

    nfcScreen = NFC_EMULATING;

    tft.fillScreen(displayColorBg());
    displayDrawHeaderBar("NFC Emulating");
    tft.setTextSize(1);
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(8, displayHeaderHeight() + 10);
    tft.print("UID: " + pn532UidToString(nfcEmuUid, 4));
    tft.setCursor(8, displayHeaderHeight() + 34);
    tft.print("Hold a phone near the");
    tft.setCursor(8, displayHeaderHeight() + 46);
    tft.print("antenna.");
    tft.setCursor(8, displayHeaderHeight() + 70);
    tft.print("OK = new UID");
    displayDrawFooterBar("BACK stop");

    uint32_t start = millis();
    while (millis() - start < 30000) {
        int evt = buttonsPoll();
        if (evt == EVT_BACK) break;
        if (evt == EVT_OK) {
            nfcEmuUid[3] = (nfcEmuUid[3] + 1) & 0xFF;
            nfcEmu->setUid(nfcEmuUid);
            nfcEmu->init();
            tft.fillRect(0, displayHeaderHeight() + 8,
                         tft.width(), 24, displayColorBg());
            tft.setTextColor(displayColorFg(), displayColorBg());
            tft.setCursor(8, displayHeaderHeight() + 10);
            tft.print("UID: " + pn532UidToString(nfcEmuUid, 4));
        }
        nfcEmu->emulate(400);
    }

    nfcScreen = NFC_MENU;
    drawNfcMenu();
}

// ============================================================
//  Feature: firmware info
// ============================================================
static void nfcShowFirmware() {
    uint32_t v = pn532GetFirmwareVersion();
    char buf[96];
    snprintf(buf, sizeof(buf),
             "Chip:     PN5%02X\n"
             "Firmware: %d.%d\n"
             "Support:  0x%02X",
             (v >> 24) & 0xFF,
             (v >> 16) & 0xFF,
             (v >>  8) & 0xFF,
              v        & 0xFF);
    drawNfcMessage("NFC Firmware", String(buf));
}

// ============================================================
//  Public entry
// ============================================================
void gpioEnterNFC() {
    nfcScreen = NFC_MENU;

    if (!pn532Begin()) {
        drawNfcMessage("NFC",
            "PN532 not found.\n\n"
            "Check wiring:\n"
            "  IO1 = SDA\n"
            "  IO2 = SCL\n\n"
            "And the I2C/SPI/HSU\n"
            "switch on the PN532\n"
            "board must be on I2C.");
        return;
    }
    drawNfcMenu();
}

void gpioHandleNFCEvent(int evt) {
    if (evt == EVT_NONE) return;
    if (nfcScreen == NFC_EMULATING) return;   // handled inline

    if (nfcScreen == NFC_MENU) {
        if (evt == EVT_UP)   { nfcMenu.up();   drawNfcMenu(); return; }
        if (evt == EVT_DOWN) { nfcMenu.down(); drawNfcMenu(); return; }
        if (evt == EVT_OK) {
            switch (nfcMenu.index()) {
                case 0: nfcReadTagInfo();    break;
                case 1: nfcStartEmulation(); break;
                case 2: nfcShowFirmware();   break;
            }
            return;
        }
        if (evt == EVT_BACK) {
            enterState(STATE_GPIO_MENU);
        }
        return;
    }

    if (evt == EVT_BACK) {
        nfcScreen = NFC_MENU;
        drawNfcMenu();
    } else if (evt == EVT_OK) {
        if (nfcMenu.index() == 0) nfcReadTagInfo();
        else if (nfcMenu.index() == 2) nfcShowFirmware();
    }
}