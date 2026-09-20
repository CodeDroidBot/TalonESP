#include "pn532_rfid.h"
#include "pn532_common.h"
#include "config.h"
#include "display.h"
#include "buttons.h"
#include "menu.h"
#include "gpio_module.h"
#include "sdcard.h"
#include <SD.h>

extern void enterState(int stateId);

// ============================================================
//  Constants & state
// ============================================================
static const uint8_t DEFAULT_KEY_A[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static const uint8_t MIFARE_1K_BLOCKS   = 64;

static SimpleMenu rfidMenu({
    "Read UID",
    "Read Block",
    "Write Block",
    "Dump Card to SD",
    "Clone Card",
    "Test Default Keys"
});

static uint8_t rfidUid[7] = {0};
static uint8_t rfidUidLen = 0;
static uint8_t rfidBlockNum = 0;
static bool    rfidHaveCard = false;

// ============================================================
//  Drawing
// ============================================================
static void drawRfidMenu() {
    displayShowMenu("RFID Reader", rfidMenu.items(), rfidMenu.index());
}

static void drawRfidMessage(const char* title, const String& msg) {
    tft.fillScreen(displayColorBg());
    displayDrawHeaderBar(title);
    tft.setTextSize(1);
    tft.setTextColor(displayColorFg(), displayColorBg());
    int y = displayHeaderHeight() + 6;
    int start = 0;
    while (start < (int)msg.length() && y < tft.height() - 22) {
        int nl = msg.indexOf('\n', start);
        String line = (nl < 0) ? msg.substring(start) : msg.substring(start, nl);
        start = (nl < 0) ? msg.length() : nl + 1;
        tft.setCursor(8, y);
        tft.print(line);
        y += 11;
    }
    displayDrawFooterBar("OK retry  BACK exit");
}

// ============================================================
//  Helpers
// ============================================================
static bool rfidAuthBlock(uint8_t block) {
    return nfc.mifareclassic_AuthenticateBlock(
        rfidUid, rfidUidLen, block, 0, (uint8_t*)DEFAULT_KEY_A);
}

static bool rfidTryUid() {
    if (rfidHaveCard && rfidUidLen > 0) return true;
    rfidUidLen = 0;
    if (pn532ReadUid(rfidUid, rfidUidLen, 3000)) {
        rfidHaveCard = true;
        return true;
    }
    return false;
}

// ============================================================
//  Feature: read UID
// ============================================================
static void rfidReadUid() {
    rfidHaveCard = false;
    rfidUidLen = 0;

    if (!pn532ReadUid(rfidUid, rfidUidLen, 4000)) {
        drawRfidMessage("RFID Read UID",
            "No card detected.\n\n"
            "Hold a Mifare Classic,\n"
            "Ultralight, or NTAG near\n"
            "the antenna.");
        return;
    }
    rfidHaveCard = true;

    String info = "UID:    " + pn532UidToString(rfidUid, rfidUidLen) + "\n";
    info += "Length: " + String(rfidUidLen) + " bytes\n";

    uint8_t page3[4] = {0};
    if (nfc.mifareultralight_ReadPage(3, page3)) {
        info += "Type:   NTAG-family\n";
    } else if (rfidUidLen == 4) {
        info += "Type:   Mifare Classic 1K\n";
    }

    drawRfidMessage("RFID UID", info);
}

// ============================================================
//  Feature: read block
// ============================================================
static void rfidReadBlock() {
    if (!rfidTryUid()) {
        drawRfidMessage("RFID Read Block", "No card detected.");
        return;
    }

    if (!rfidAuthBlock(rfidBlockNum)) {
        drawRfidMessage("RFID Read Block",
            "Auth failed on block\n" + String(rfidBlockNum) + ".\n\n"
            "Tried default key A:\nFF FF FF FF FF FF\n\n"
            "Try 'Test Default Keys'\nto find the right key.");
        return;
    }

    uint8_t data[16] = {0};
    if (!nfc.mifareclassic_ReadDataBlock(rfidBlockNum, data)) {
        drawRfidMessage("RFID Read Block", "Read failed.");
        return;
    }

    String msg = "UID:   " + pn532UidToString(rfidUid, rfidUidLen) + "\n";
    msg += "Block: " + String(rfidBlockNum) + "\n\n";
    for (uint8_t row = 0; row < 2; row++) {
        for (uint8_t i = 0; i < 8; i++) {
            uint8_t b = data[row * 8 + i];
            if (b < 0x10) msg += "0";
            msg += String(b, HEX);
            msg += " ";
        }
        msg += "\n";
    }
    msg += "\nUP/DN change block";
    drawRfidMessage("RFID Block", msg);
}

// ============================================================
//  Feature: write block
// ============================================================
static void rfidWriteBlock() {
    if (!rfidTryUid()) {
        drawRfidMessage("RFID Write", "No card detected.");
        return;
    }

    if (!rfidAuthBlock(rfidBlockNum)) {
        drawRfidMessage("RFID Write",
            "Auth failed on block\n" + String(rfidBlockNum));
        return;
    }

    // Incrementing test pattern so writes are visible on re-read.
    uint8_t data[16];
    for (uint8_t i = 0; i < 16; i++) data[i] = rfidBlockNum + i;

    if (!nfc.mifareclassic_WriteDataBlock(rfidBlockNum, data)) {
        drawRfidMessage("RFID Write", "Write failed.");
        return;
    }

    String msg = "Wrote 16 bytes to\nblock " + String(rfidBlockNum) + ".\n\n";
    msg += "Data: ";
    for (uint8_t i = 0; i < 16 && i < 8; i++) {
        if (data[i] < 0x10) msg += "0";
        msg += String(data[i], HEX);
        msg += " ";
    }
    msg += "\n";
    drawRfidMessage("RFID Write", msg);
}

// ============================================================
//  Feature: dump card to SD
// ============================================================
static void rfidDumpCard() {
    if (!rfidTryUid()) {
        drawRfidMessage("RFID Dump", "No card detected.");
        return;
    }

    tft.fillScreen(displayColorBg());
    displayDrawHeaderBar("RFID Dump");
    tft.setTextSize(1);
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(8, displayHeaderHeight() + 10);
    tft.print("Reading blocks...");

    // Build path — replace ':' (invalid on FAT) with '-'
    String uidStr = pn532UidToString(rfidUid, rfidUidLen);
    uidStr.replace(":", "-");
    String path = "/rfid/dump_" + uidStr + ".txt";

    if (!SD.exists("/rfid")) SD.mkdir("/rfid");
    File f = SD.open(path.c_str(), FILE_WRITE);
    if (!f) {
        drawRfidMessage("RFID Dump", "Could not open file\non SD card.");
        return;
    }

    f.printf("# Mifare Classic dump\n");
    f.printf("# UID: %s\n", pn532UidToString(rfidUid, rfidUidLen).c_str());
    f.printf("# Blocks: %u\n\n", MIFARE_1K_BLOCKS);

    int ok = 0, failed = 0;
    for (uint8_t block = 0; block < MIFARE_1K_BLOCKS; block++) {
        uint8_t data[16] = {0};
        bool authed  = rfidAuthBlock(block);
        bool read_ok = authed && nfc.mifareclassic_ReadDataBlock(block, data);

        f.printf("Block %02u: ", block);
        if (read_ok) {
            for (uint8_t i = 0; i < 16; i++) f.printf("%02X ", data[i]);
            ok++;
        } else {
            for (uint8_t i = 0; i < 16; i++) f.print("-- ");
            failed++;
        }
        f.print("\n");

        // Progress bar
        int barY = tft.height() - 50;
        int pct  = ((block + 1) * 100) / MIFARE_1K_BLOCKS;
        tft.drawRect(10, barY, tft.width() - 20, 12, displayColorFgDim());
        tft.fillRect(12, barY + 2,
                     ((tft.width() - 24) * pct) / 100, 8, TFT_GREEN);
    }
    f.close();

    String msg = "Dump complete.\n";
    msg += String(ok) + " blocks read\n";
    msg += String(failed) + " blocks failed\n\n";
    msg += "Saved to:\n" + path;
    drawRfidMessage("RFID Dump", msg);
}

// ============================================================
//  Feature: clone card
// ============================================================
static void rfidCloneCard() {
    drawRfidMessage("RFID Clone",
        "1. Place SOURCE card\n"
        "   on the antenna.\n\n"
        "   Press OK when ready.");

    while (true) {
        int evt = buttonsPoll();
        if (evt == EVT_BACK) { drawRfidMenu(); return; }
        if (evt == EVT_OK) break;
        delay(50);
    }

    uint8_t srcUid[7] = {0};
    uint8_t srcUidLen = 0;
    if (!pn532ReadUid(srcUid, srcUidLen, 3000)) {
        drawRfidMessage("RFID Clone", "No source card\nfound.");
        return;
    }

    // Save source UID for auth on source
    memcpy(rfidUid, srcUid, 7);
    rfidUidLen = srcUidLen;

    // Read all blocks
    static uint8_t blocks[MIFARE_1K_BLOCKS][16];
    int read_ok = 0;
    for (uint8_t b = 0; b < MIFARE_1K_BLOCKS; b++) {
        memset(blocks[b], 0, 16);
        if (rfidAuthBlock(b) && nfc.mifareclassic_ReadDataBlock(b, blocks[b])) {
            read_ok++;
        }
    }

    drawRfidMessage("RFID Clone",
        String(read_ok) + "/64 blocks read.\n\n"
        "2. Remove SOURCE and\n"
        "   place TARGET card.\n\n"
        "   Press OK to write.");

    while (true) {
        int evt = buttonsPoll();
        if (evt == EVT_BACK) { drawRfidMenu(); return; }
        if (evt == EVT_OK) break;
        delay(50);
    }

    uint8_t tgtUid[7] = {0};
    uint8_t tgtUidLen = 0;
    if (!pn532ReadUid(tgtUid, tgtUidLen, 3000)) {
        drawRfidMessage("RFID Clone", "No target card\nfound.");
        return;
    }

    // Auth uses target's UID
    memcpy(rfidUid, tgtUid, 7);
    rfidUidLen = tgtUidLen;

    // Write all blocks except block 0 (UID / manufacturer block,
    // which is locked on non-magic cards)
    int wrote = 0;
    for (uint8_t b = 1; b < MIFARE_1K_BLOCKS; b++) {
        if (rfidAuthBlock(b) && nfc.mifareclassic_WriteDataBlock(b, blocks[b])) {
            wrote++;
        }
    }

    drawRfidMessage("RFID Clone",
        "Wrote " + String(wrote) + "/63 blocks.\n\n"
        "Note: block 0 (UID) was\n"
        "skipped — only 'magic'\n"
        "cards support UID change.");
}

// ============================================================
//  Feature: test default keys
// ============================================================
static void rfidTestKeys() {
    if (!pn532ReadUid(rfidUid, rfidUidLen, 3000)) {
        drawRfidMessage("RFID Keys", "No card detected.");
        return;
    }
    rfidHaveCard = true;

    struct { const char* label; uint8_t key[6]; } keys[] = {
        {"FF FF FF FF FF FF", {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}},
        {"A0 A1 A2 A3 A4 A5", {0xA0,0xA1,0xA2,0xA3,0xA4,0xA5}},
        {"D3 F7 D3 F7 D3 F7", {0xD3,0xF7,0xD3,0xF7,0xD3,0xF7}},
        {"00 00 00 00 00 00", {0x00,0x00,0x00,0x00,0x00,0x00}},
    };

    String msg = "Testing key A on\nblock 0:\n\n";
    for (auto& k : keys) {
        bool ok = nfc.mifareclassic_AuthenticateBlock(
            rfidUid, rfidUidLen, 0, 0, (uint8_t*)k.key);
        msg += String(k.label) + "\n  -> " + (ok ? "OK" : "fail") + "\n\n";
    }
    drawRfidMessage("RFID Keys", msg);
}

// ============================================================
//  Public entry
// ============================================================
void gpioEnterRFID() {
    rfidHaveCard = false;
    rfidUidLen = 0;
    rfidBlockNum = 0;

    if (!pn532Begin()) {
        drawRfidMessage("RFID",
            "PN532 not found.\n\n"
            "Check wiring:\n"
            "  IO1 = SDA\n"
            "  IO2 = SCL\n\n"
            "And the I2C/SPI/HSU\n"
            "switch on the PN532\n"
            "board — it must be on\n"
            "I2C.");
        return;
    }
    drawRfidMenu();
}

void gpioHandleRFIDEvent(int evt) {
    if (evt == EVT_NONE) return;

    // Menu is the only "navigation" screen. Everything else is
    // a blocking message screen that returns to the menu on BACK.
    if (rfidMenu.index() == 99) return;   // never true; keeps compiler happy

    // We can't tell which sub-screen we're on without tracking it,
    // so we use a simple rule: if BACK, go to menu; if OK, retry.
    static bool inSubScreen = false;

    if (!inSubScreen) {
        if (evt == EVT_UP)   { rfidMenu.up();   drawRfidMenu(); return; }
        if (evt == EVT_DOWN) { rfidMenu.down(); drawRfidMenu(); return; }
        if (evt == EVT_OK) {
            inSubScreen = true;
            switch (rfidMenu.index()) {
                case 0: rfidReadUid();   inSubScreen = false; break;
                case 1: rfidReadBlock(); inSubScreen = false; break;
                case 2: rfidWriteBlock(); inSubScreen = false; break;
                case 3: rfidDumpCard();  inSubScreen = false; break;
                case 4: rfidCloneCard(); inSubScreen = false; break;
                case 5: rfidTestKeys();  inSubScreen = false; break;
            }
            return;
        }
        if (evt == EVT_BACK) {
            enterState(STATE_GPIO_MENU);
        }
        return;
    }

    if (evt == EVT_BACK) {
        inSubScreen = false;
        drawRfidMenu();
    }
}