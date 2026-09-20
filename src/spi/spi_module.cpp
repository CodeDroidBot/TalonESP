#include "spi_module.h"
#include "config.h"
#include "display.h"
#include "buttons.h"
#include "menu.h"
#include "gpio_module.h"
#include "keyboard/keyboard_ui.h"
#include "services/services.h"

extern void enterState(int stateId);

static SimpleMenu spiActionMenu({"Probe", "Transfer", "Mode", "Freq", "Slave"});
static uint8_t spiMode = 0;
static uint32_t spiFreq = 1000000;

// ============================================================
//  FIX: same undefined-behavior bug as in i2c_module.cpp.
//  Rewritten without mutating the source String buffer.
// ============================================================
static std::vector<uint8_t> parseHexBytes(const String& str) {
    std::vector<uint8_t> bytes;
    int len = str.length();
    int i = 0;
    while (i < len) {
        while (i < len && isspace((unsigned char)str[i])) i++;
        if (i >= len) break;
        int start = i;
        while (i < len && !isspace((unsigned char)str[i])) i++;
        String tok = str.substring(start, i);
        bytes.push_back((uint8_t)strtol(tok.c_str(), nullptr, 16));
    }
    return bytes;
}

void gpioEnterSPIMenu() {
    SPI_SERVICE.end();
    displayShowMenu("GPIO - SPI", spiActionMenu.items(), spiActionMenu.index());
}

void gpioHandleSPIMenuEvent(int evt) {
    if (evt == EVT_UP)   { spiActionMenu.up();   gpioEnterSPIMenu(); return; }
    if (evt == EVT_DOWN) { spiActionMenu.down(); gpioEnterSPIMenu(); return; }
    if (evt == EVT_OK) {
        switch (spiActionMenu.index()) {
            case 0: enterState(STATE_GPIO_SPI_PROBE); break;
            case 1: enterState(STATE_SPI_TRANSFER);   break;
            case 2: enterState(STATE_SPI_MODE);       break;
            case 3: enterState(STATE_SPI_FREQ);       break;
            case 4: enterState(STATE_SPI_SLAVE);      break;
        }
    }
}

void gpioEnterSPIProbe() {
    SPI_SERVICE.begin(PIN_GPIO_IO1, PIN_GPIO_IO2, PIN_GPIO_IO3, PIN_GPIO_IO4);
    SPI_SERVICE.setMode(0);
    SPI_SERVICE.setFrequency(1000000);
    SPI_SERVICE.csLow();
    uint8_t mfg  = SPI_SERVICE.transfer(0x9F);
    uint8_t type = SPI_SERVICE.transfer(0x00);
    uint8_t cap  = SPI_SERVICE.transfer(0x00);
    SPI_SERVICE.csHigh();
    SPI_SERVICE.end();

    char buf[96];
    if (mfg == 0x00 || mfg == 0xFF) {
        snprintf(buf, sizeof(buf), "No response.\nCheck wiring:\nIO1=SCK IO2=MOSI\nIO3=MISO IO4=CS");
    } else {
        snprintf(buf, sizeof(buf), "Mfg:  0x%02X\nType: 0x%02X\nCap:  0x%02X\n(JEDEC ID, opcode 0x9F)",
                 mfg, type, cap);
    }
    displayShowMessage("SPI Probe (OK=retry)", buf);
}

void gpioHandleSPIProbeEvent(int evt) {
    if (evt == EVT_OK) gpioEnterSPIProbe();
}

void spiEnterTransfer() {
    OnScreenKeyboardConfig cfg;
    osKeyboardUseStandardLayout(cfg);
    cfg.title = "SPI Transfer";
    cfg.subtitle = "Enter bytes (hex, space-sep)";
    cfg.maxLen = 30;
    cfg.okLabel = "Send";
    cfg.requireNonEmpty = true;
    String dataStr;
    osKeyboardEnter(cfg, dataStr);
    if (osKeyboardWasCancelled()) { enterState(STATE_GPIO_SPI_MENU); return; }

    auto tx = parseHexBytes(dataStr);
    if (tx.empty()) {
        displayShowMessage("SPI", "No bytes entered.");
        delay(800);
        enterState(STATE_GPIO_SPI_MENU);
        return;
    }

    SPI_SERVICE.begin(PIN_GPIO_IO1, PIN_GPIO_IO2, PIN_GPIO_IO3, PIN_GPIO_IO4);
    SPI_SERVICE.setMode(spiMode);
    SPI_SERVICE.setFrequency(spiFreq);
    std::vector<uint8_t> rx(tx.size());
    SPI_SERVICE.csLow();
    SPI_SERVICE.transfer(tx.data(), rx.data(), tx.size());
    SPI_SERVICE.csHigh();
    SPI_SERVICE.end();

    String result = "TX: ";
    for (auto b : tx) { char buf[4]; sprintf(buf, "%02X ", b); result += buf; }
    result += "\nRX: ";
    for (auto b : rx) { char buf[4]; sprintf(buf, "%02X ", b); result += buf; }
    displayShowMessage("SPI Transfer", result.c_str());
    delay(2000);
    enterState(STATE_GPIO_SPI_MENU);
}

void spiEnterMode() {
    OnScreenKeyboardConfig cfg;
    osKeyboardUseStandardLayout(cfg);
    cfg.title = "SPI Mode";
    cfg.subtitle = "Enter mode (0-3)";
    cfg.maxLen = 1;
    cfg.okLabel = "Set";
    cfg.requireNonEmpty = true;
    String modeStr;
    osKeyboardEnter(cfg, modeStr);
    if (osKeyboardWasCancelled()) { enterState(STATE_GPIO_SPI_MENU); return; }
    uint8_t m = strtol(modeStr.c_str(), nullptr, 10);
    if (m <= 3) {
        spiMode = m;
        String msg = "Mode set to " + String(m);
        displayShowMessage("SPI", msg.c_str());
    } else {
        displayShowMessage("SPI", "Invalid mode (0-3)");
    }
    delay(800);
    enterState(STATE_GPIO_SPI_MENU);
}

void spiEnterFreq() {
    OnScreenKeyboardConfig cfg;
    osKeyboardUseStandardLayout(cfg);
    cfg.title = "SPI Frequency";
    cfg.subtitle = "Enter kHz (e.g. 1000)";
    cfg.maxLen = 6;
    cfg.okLabel = "Set";
    cfg.requireNonEmpty = true;
    String freqStr;
    osKeyboardEnter(cfg, freqStr);
    if (osKeyboardWasCancelled()) { enterState(STATE_GPIO_SPI_MENU); return; }
    uint32_t freq = strtol(freqStr.c_str(), nullptr, 10) * 1000;
    if (freq >= 100000 && freq <= 10000000) {
        spiFreq = freq;
        String msg = "Freq set to " + String(freq / 1000) + " kHz";
        displayShowMessage("SPI", msg.c_str());
    } else {
        displayShowMessage("SPI", "Invalid freq (100..10000 kHz)");
    }
    delay(800);
    enterState(STATE_GPIO_SPI_MENU);
}

void spiEnterSlave() {
    displayShowMessage("SPI Slave", "Slave mode not yet implemented.\nCheck back later.");
    delay(1000);
    enterState(STATE_GPIO_SPI_MENU);
}