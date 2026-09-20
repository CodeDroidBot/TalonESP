#include "i2c_module.h"
#include "config.h"
#include "display.h"
#include "buttons.h"
#include "menu.h"
#include "gpio_module.h"
#include "keyboard/keyboard_ui.h"
#include "services/services.h"

extern void enterState(int stateId);

static SimpleMenu i2cActionMenu({"Scan", "Read", "Write", "Write+Read", "Speed", "Slave"});
static uint8_t i2cSpeed = 100; // kHz

// ============================================================
//  FIX: previously cast away the const-ness of String::c_str()
//  and let strtok() write null terminators into the buffer, which
//  is undefined behavior on Arduino's reference-counted String and
//  could corrupt other live Strings that shared the same buffer.
//
//  New version parses without modifying the source string and
//  without strtok. Handles whitespace-separated hex tokens.
// ============================================================
static std::vector<uint8_t> parseHexBytes(const String& str) {
    std::vector<uint8_t> bytes;
    int len = str.length();
    int i = 0;
    while (i < len) {
        // Skip whitespace
        while (i < len && isspace((unsigned char)str[i])) i++;
        if (i >= len) break;

        // Read one hex token
        int start = i;
        while (i < len && !isspace((unsigned char)str[i])) i++;
        String tok = str.substring(start, i);
        bytes.push_back((uint8_t)strtol(tok.c_str(), nullptr, 16));
    }
    return bytes;
}

void gpioEnterI2CMenu() {
    I2C_SERVICE.end();
    displayShowMenu("GPIO - I2C", i2cActionMenu.items(), i2cActionMenu.index());
}

void gpioHandleI2CMenuEvent(int evt) {
    if (evt == EVT_UP)   { i2cActionMenu.up();   gpioEnterI2CMenu(); return; }
    if (evt == EVT_DOWN) { i2cActionMenu.down(); gpioEnterI2CMenu(); return; }
    if (evt == EVT_OK) {
        switch (i2cActionMenu.index()) {
            case 0: enterState(STATE_GPIO_I2C_SCAN); break;
            case 1: enterState(STATE_I2C_READ);      break;
            case 2: enterState(STATE_I2C_WRITE);     break;
            case 3: enterState(STATE_I2C_WRITEREAD); break;
            case 4: enterState(STATE_I2C_SPEED);     break;
            case 5: enterState(STATE_I2C_SLAVE);     break;
        }
    }
}

void gpioEnterI2CScan() {
    I2C_SERVICE.begin(PIN_GPIO_IO1, PIN_GPIO_IO2, i2cSpeed * 1000);
    tft.fillScreen(displayColorBg());
    displayDrawHeaderBar("I2C Scan (IO1=SDA IO2=SCL)");
    tft.setTextSize(1);
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(10, displayHeaderHeight() + 10);
    tft.print("Scanning...");

    String found = "";
    int count = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (I2C_SERVICE.isDeviceReady(addr)) {
            char buf[8];
            snprintf(buf, sizeof(buf), "0x%02X ", addr);
            found += buf;
            count++;
        }
    }
    I2C_SERVICE.end();

    String msg = count ? (String(count) + " device(s):\n" + found) : "No devices found.";
    displayShowMessage("I2C Scan (OK=rescan)", msg.c_str());
}

void gpioHandleI2CScanEvent(int evt) {
    if (evt == EVT_OK) gpioEnterI2CScan();
}

void i2cEnterRead() {
    OnScreenKeyboardConfig cfg;
    osKeyboardUseStandardLayout(cfg);
    cfg.title = "I2C Read";
    cfg.subtitle = "Enter address (hex, e.g. 50)";
    cfg.maxLen = 2;
    cfg.okLabel = "Next";
    cfg.requireNonEmpty = true;
    String addrStr;
    osKeyboardEnter(cfg, addrStr);
    if (osKeyboardWasCancelled()) { enterState(STATE_GPIO_I2C_MENU); return; }
    uint8_t addr = strtol(addrStr.c_str(), nullptr, 16);

    cfg.title = "I2C Read";
    cfg.subtitle = "Enter # of bytes (dec)";
    cfg.maxLen = 3;
    cfg.okLabel = "Read";
    cfg.requireNonEmpty = true;
    String lenStr;
    osKeyboardEnter(cfg, lenStr);
    if (osKeyboardWasCancelled()) { enterState(STATE_GPIO_I2C_MENU); return; }
    uint8_t len = strtol(lenStr.c_str(), nullptr, 10);
    if (len > 64) len = 64;

    I2C_SERVICE.begin(PIN_GPIO_IO1, PIN_GPIO_IO2, i2cSpeed * 1000);
    if (!I2C_SERVICE.isDeviceReady(addr)) {
        displayShowMessage("I2C", "Address not responding.");
        delay(800);
        I2C_SERVICE.end();
        enterState(STATE_GPIO_I2C_MENU);
        return;
    }
    uint8_t data[64];
    int got = I2C_SERVICE.readBytes(addr, data, len);
    I2C_SERVICE.end();

    String result = "Read " + String(got) + " bytes:\n";
    for (int i = 0; i < got; i++) {
        char buf[4]; sprintf(buf, "%02X ", data[i]);
        result += buf;
        if ((i + 1) % 16 == 0) result += "\n";
    }
    displayShowMessage("I2C Read", result.c_str());
    delay(2000);
    enterState(STATE_GPIO_I2C_MENU);
}

void i2cEnterWrite() {
    OnScreenKeyboardConfig cfg;
    osKeyboardUseStandardLayout(cfg);
    cfg.title = "I2C Write";
    cfg.subtitle = "Enter address (hex)";
    cfg.maxLen = 2;
    cfg.okLabel = "Next";
    cfg.requireNonEmpty = true;
    String addrStr;
    osKeyboardEnter(cfg, addrStr);
    if (osKeyboardWasCancelled()) { enterState(STATE_GPIO_I2C_MENU); return; }
    uint8_t addr = strtol(addrStr.c_str(), nullptr, 16);

    cfg.title = "I2C Write";
    cfg.subtitle = "Enter bytes (hex, space-sep)";
    cfg.maxLen = 30;
    cfg.okLabel = "Write";
    cfg.requireNonEmpty = true;
    String dataStr;
    osKeyboardEnter(cfg, dataStr);
    if (osKeyboardWasCancelled()) { enterState(STATE_GPIO_I2C_MENU); return; }
    auto bytes = parseHexBytes(dataStr);
    if (bytes.empty()) {
        displayShowMessage("I2C", "No bytes entered.");
        delay(800);
        enterState(STATE_GPIO_I2C_MENU);
        return;
    }

    I2C_SERVICE.begin(PIN_GPIO_IO1, PIN_GPIO_IO2, i2cSpeed * 1000);
    uint8_t err = I2C_SERVICE.writeBytes(addr, bytes.data(), bytes.size());
    I2C_SERVICE.end();

    String msg = (err == 0) ? "Write successful." : "Write failed (error " + String(err) + ")";
    displayShowMessage("I2C", msg.c_str());
    delay(1000);
    enterState(STATE_GPIO_I2C_MENU);
}

void i2cEnterWriteRead() {
    OnScreenKeyboardConfig cfg;
    osKeyboardUseStandardLayout(cfg);
    cfg.title = "I2C Write+Read";
    cfg.subtitle = "Address (hex)";
    cfg.maxLen = 2;
    cfg.okLabel = "Next";
    cfg.requireNonEmpty = true;
    String addrStr;
    osKeyboardEnter(cfg, addrStr);
    if (osKeyboardWasCancelled()) { enterState(STATE_GPIO_I2C_MENU); return; }
    uint8_t addr = strtol(addrStr.c_str(), nullptr, 16);

    cfg.title = "I2C Write+Read";
    cfg.subtitle = "Write bytes (hex)";
    cfg.maxLen = 30;
    cfg.okLabel = "Next";
    cfg.requireNonEmpty = true;
    String wdata;
    osKeyboardEnter(cfg, wdata);
    if (osKeyboardWasCancelled()) { enterState(STATE_GPIO_I2C_MENU); return; }
    auto wbytes = parseHexBytes(wdata);

    cfg.title = "I2C Write+Read";
    cfg.subtitle = "# of bytes to read";
    cfg.maxLen = 3;
    cfg.okLabel = "Do It";
    cfg.requireNonEmpty = true;
    String rlenStr;
    osKeyboardEnter(cfg, rlenStr);
    if (osKeyboardWasCancelled()) { enterState(STATE_GPIO_I2C_MENU); return; }
    uint8_t rlen = strtol(rlenStr.c_str(), nullptr, 10);
    if (rlen > 64) rlen = 64;

    I2C_SERVICE.begin(PIN_GPIO_IO1, PIN_GPIO_IO2, i2cSpeed * 1000);
    uint8_t rdata[64];
    bool ok = I2C_SERVICE.writeThenRead(addr, wbytes.data(), wbytes.size(), rdata, rlen);
    I2C_SERVICE.end();

    if (!ok) {
        displayShowMessage("I2C", "Operation failed.");
        delay(800);
        enterState(STATE_GPIO_I2C_MENU);
        return;
    }
    String result = "Read " + String(rlen) + " bytes:\n";
    for (int i = 0; i < rlen; i++) {
        char buf[4]; sprintf(buf, "%02X ", rdata[i]);
        result += buf;
        if ((i + 1) % 16 == 0) result += "\n";
    }
    displayShowMessage("I2C Write+Read", result.c_str());
    delay(2000);
    enterState(STATE_GPIO_I2C_MENU);
}

void i2cEnterSpeed() {
    OnScreenKeyboardConfig cfg;
    osKeyboardUseStandardLayout(cfg);
    cfg.title = "I2C Speed";
    cfg.subtitle = "Enter kHz (100 or 400)";
    cfg.maxLen = 3;
    cfg.okLabel = "Set";
    cfg.requireNonEmpty = true;
    String spdStr;
    osKeyboardEnter(cfg, spdStr);
    if (osKeyboardWasCancelled()) { enterState(STATE_GPIO_I2C_MENU); return; }
    uint16_t spd = strtol(spdStr.c_str(), nullptr, 10);
    if (spd == 100 || spd == 400) {
        i2cSpeed = spd;
        I2C_SERVICE.setSpeed(spd * 1000);
        String msg = "Speed set to " + String(spd) + " kHz";
        displayShowMessage("I2C", msg.c_str());
    } else {
        displayShowMessage("I2C", "Invalid speed (use 100 or 400)");
    }
    delay(800);
    enterState(STATE_GPIO_I2C_MENU);
}

void i2cEnterSlave() {
    I2C_SERVICE.begin(PIN_GPIO_IO1, PIN_GPIO_IO2, 100000);
    I2C_SERVICE.beginSlave(0x08);
    displayShowMessage("I2C Slave", "Slave echo active.\nAddress 0x08.\nPress BACK to exit.");
}