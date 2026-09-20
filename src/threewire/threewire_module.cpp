#include "threewire_module.h"
#include "config.h"
#include "display.h"
#include "buttons.h"
#include "menu.h"
#include "gpio_module.h"
#include "keyboard/keyboard_ui.h"

extern void enterState(int stateId);

static SimpleMenu threewireMenu({"Read", "Write", "Erase", "Write Enable", "Write Disable"});

// Pins: IO1=CLK, IO2=DATA, IO3=CS
static uint8_t pinCLK  = PIN_GPIO_IO1;
static uint8_t pinDATA = PIN_GPIO_IO2;
static uint8_t pinCS   = PIN_GPIO_IO3;

static void clk_high() { digitalWrite(pinCLK, HIGH); }
static void clk_low()  { digitalWrite(pinCLK, LOW); }
static void data_high(){ digitalWrite(pinDATA, HIGH); }
static void data_low() { digitalWrite(pinDATA, LOW); }
static int  data_read(){ return digitalRead(pinDATA); }
static void cs_high()  { digitalWrite(pinCS, HIGH); }
static void cs_low()   { digitalWrite(pinCS, LOW); }

static void delay_us(uint16_t us) { delayMicroseconds(us); }

static void sendByte(uint8_t b) {
    for (int i = 7; i >= 0; i--) {
        digitalWrite(pinDATA, (b >> i) & 1 ? HIGH : LOW);
        clk_high();
        delay_us(1);
        clk_low();
        delay_us(1);
    }
}

static uint8_t recvByte() {
    uint8_t b = 0;
    for (int i = 7; i >= 0; i--) {
        clk_high();
        delay_us(1);
        if (digitalRead(pinDATA)) b |= (1 << i);
        clk_low();
        delay_us(1);
    }
    return b;
}

static void startCmd() { cs_low();  delay_us(1); }
static void endCmd()   { cs_high(); delay_us(1); }

// ---- 93Cxx commands (16-bit organization) ----
#define CMD_READ  0x6
#define CMD_WRITE 0x5
#define CMD_EWEN  0x4
#define CMD_EWDS  0x4
#define CMD_ERASE 0x7
#define CMD_ERAL  0x4
#define CMD_WRAL  0x4

// Address length for 93C46 = 6 bits, 93C56 = 8, 93C66 = 9, etc.
static uint8_t addrLen = 6; // default 93C46

// ============================================================
//  FIX: previously threewireWriteEnable() showed a message and
//  returned to the menu — but Write and Erase both called it as
//  their first step, so the user got bounced to the menu before
//  the actual operation ran. Now the enable sequence is a silent
//  helper, and the user-facing menu item wraps it with UI.
// ============================================================
static void threewireWriteEnableSilent() {
    pinMode(pinCLK, OUTPUT);
    pinMode(pinDATA, OUTPUT);
    pinMode(pinCS, OUTPUT);
    cs_high();
    startCmd();
    sendByte(0b10011000);   // Start + EWEN opcode (0x4 + opcode 0x00)
    sendByte(0b11000000);   // 8 dummy bits
    endCmd();
}

void threewireEnterMenu() {
    displayShowMenu("GPIO - 3-Wire", threewireMenu.items(), threewireMenu.index());
}

void threewireHandleMenuEvent(int evt) {
    if (evt == EVT_UP)   { threewireMenu.up();   threewireEnterMenu(); return; }
    if (evt == EVT_DOWN) { threewireMenu.down(); threewireEnterMenu(); return; }
    if (evt == EVT_OK) {
        switch (threewireMenu.index()) {
            case 0: enterState(STATE_THREEWIRE_READ);  break;
            case 1: enterState(STATE_THREEWIRE_WRITE); break;
            case 2: enterState(STATE_THREEWIRE_ERASE); break;
            case 3: threewireWriteEnable();            break;
            case 4: threewireWriteDisable();           break;
        }
    }
}

// User-facing menu action — silent helper + UI feedback
void threewireWriteEnable() {
    threewireWriteEnableSilent();
    displayShowMessage("3-Wire", "Write Enabled");
    delay(800);
    threewireEnterMenu();
}

void threewireWriteDisable() {
    pinMode(pinCLK, OUTPUT);
    pinMode(pinDATA, OUTPUT);
    pinMode(pinCS, OUTPUT);
    cs_high();
    startCmd();
    sendByte(0b10000000);   // Start + EWDS opcode
    sendByte(0b00000000);
    endCmd();
    displayShowMessage("3-Wire", "Write Disabled");
    delay(800);
    threewireEnterMenu();
}

void threewireEnterRead() {
    OnScreenKeyboardConfig cfg;
    osKeyboardUseStandardLayout(cfg);
    cfg.title = "3-Wire Read";
    cfg.subtitle = "Enter address (dec)";
    cfg.maxLen = 3;
    cfg.okLabel = "Read";
    cfg.requireNonEmpty = true;
    String addrStr;
    osKeyboardEnter(cfg, addrStr);
    if (osKeyboardWasCancelled()) { threewireEnterMenu(); return; }
    uint16_t addr = strtol(addrStr.c_str(), nullptr, 10);

    pinMode(pinCLK, OUTPUT);
    pinMode(pinDATA, OUTPUT);
    pinMode(pinCS, OUTPUT);
    cs_high();
    startCmd();
    sendByte(0b10000000 | (CMD_READ << 5) | (addr >> (addrLen - 4)));
    uint8_t rem = addr & ((1 << (addrLen - 4)) - 1);
    sendByte(rem << (8 - (addrLen - 4)));
    pinMode(pinDATA, INPUT);
    uint8_t hi = recvByte();
    uint8_t lo = recvByte();
    endCmd();
    uint16_t value = ((uint16_t)hi << 8) | lo;
    char buf[24];
    sprintf(buf, "Value: 0x%04X (%d)", value, value);
    displayShowMessage("3-Wire Read", buf);
    delay(2000);
    threewireEnterMenu();
}

void threewireEnterWrite() {
    OnScreenKeyboardConfig cfg;
    osKeyboardUseStandardLayout(cfg);
    cfg.title = "3-Wire Write";
    cfg.subtitle = "Address (dec)";
    cfg.maxLen = 3;
    cfg.okLabel = "Next";
    cfg.requireNonEmpty = true;
    String addrStr;
    osKeyboardEnter(cfg, addrStr);
    if (osKeyboardWasCancelled()) { threewireEnterMenu(); return; }
    uint16_t addr = strtol(addrStr.c_str(), nullptr, 10);

    cfg.title = "3-Wire Write";
    cfg.subtitle = "Data (hex, 0-FFFF)";
    cfg.maxLen = 4;
    cfg.okLabel = "Write";
    cfg.requireNonEmpty = true;
    String dataStr;
    osKeyboardEnter(cfg, dataStr);
    if (osKeyboardWasCancelled()) { threewireEnterMenu(); return; }
    uint16_t data = strtol(dataStr.c_str(), nullptr, 16);

    // Silent enable — no UI detour
    threewireWriteEnableSilent();
    delay(10);

    pinMode(pinCLK, OUTPUT);
    pinMode(pinDATA, OUTPUT);
    pinMode(pinCS, OUTPUT);
    cs_high();
    startCmd();
    sendByte(0b10000000 | (CMD_WRITE << 5) | (addr >> (addrLen - 4)));
    uint8_t rem = addr & ((1 << (addrLen - 4)) - 1);
    sendByte(rem << (8 - (addrLen - 4)));
    sendByte((data >> 8) & 0xFF);
    sendByte(data & 0xFF);
    endCmd();
    delay(10);

    displayShowMessage("3-Wire", "Write command sent.");
    delay(1000);
    threewireEnterMenu();
}

void threewireEnterErase() {
    OnScreenKeyboardConfig cfg;
    osKeyboardUseStandardLayout(cfg);
    cfg.title = "3-Wire Erase";
    cfg.subtitle = "Address (dec)";
    cfg.maxLen = 3;
    cfg.okLabel = "Erase";
    cfg.requireNonEmpty = true;
    String addrStr;
    osKeyboardEnter(cfg, addrStr);
    if (osKeyboardWasCancelled()) { threewireEnterMenu(); return; }
    uint16_t addr = strtol(addrStr.c_str(), nullptr, 10);

    threewireWriteEnableSilent();
    delay(10);

    pinMode(pinCLK, OUTPUT);
    pinMode(pinDATA, OUTPUT);
    pinMode(pinCS, OUTPUT);
    cs_high();
    startCmd();
    sendByte(0b10000000 | (CMD_ERASE << 5) | (addr >> (addrLen - 4)));
    uint8_t rem = addr & ((1 << (addrLen - 4)) - 1);
    sendByte(rem << (8 - (addrLen - 4)));
    endCmd();
    delay(10);
    displayShowMessage("3-Wire", "Erase command sent.");
    delay(1000);
    threewireEnterMenu();
}