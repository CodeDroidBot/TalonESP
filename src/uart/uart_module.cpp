#include "uart_module.h"
#include "config.h"
#include "display.h"
#include "buttons.h"
#include "menu.h"
#include "gpio_module.h"
#include "keyboard/keyboard_ui.h"
#include "services/services.h"

extern void enterState(int stateId);

static SimpleMenu uartModeMenu({"Bridge", "Terminal", "Settings"});

// ---- Global config ----
UartConfig uartConfig = {115200, 8, 0, 1};

// ---- Bridge state ----
static bool uartBridgeActive = false;

// ---- Terminal state ----
static String terminalLog = "";
static const int MAX_TERMINAL_LINES = 8;
static String terminalLines[MAX_TERMINAL_LINES];
static int terminalLineIndex = 0;
static bool terminalActive = false;

static void drawTerminalScreen();

// ============================================================
//  FIX: UART Bridge previously used IO6 (= GPIO48) as TX, which
//  is the same pin as the onboard NeoPixel status LED. Every UART
//  byte drove that line, both garbling the LED and injecting
//  noise into the UART stream. Now TX is on IO4 (= GPIO42), a
//  free header pin.
//
//  Updated wiring:
//    IO5 = RX (ESP input from target TX)
//    IO4 = TX (ESP output to target RX)
// ============================================================

void gpioEnterUARTMenu() {
    UART_SERVICE.end();
    displayShowMenu("GPIO - UART", uartModeMenu.items(), uartModeMenu.index());
}

void gpioHandleUARTMenuEvent(int evt) {
    if (evt == EVT_UP)   { uartModeMenu.up();   gpioEnterUARTMenu(); return; }
    if (evt == EVT_DOWN) { uartModeMenu.down(); gpioEnterUARTMenu(); return; }
    if (evt == EVT_OK) {
        switch (uartModeMenu.index()) {
            case 0: enterState(STATE_GPIO_UART_ACTIVE); break;
            case 1: enterState(STATE_UART_TERMINAL);    break;
            case 2: enterState(STATE_UART_SETTINGS);    break;
        }
    }
}

void gpioEnterUARTActive() {
    terminalActive = false;
    uartBridgeActive = true;
    UART_SERVICE.begin(PIN_GPIO_IO5, PIN_GPIO_IO4, uartConfig);   // RX=IO5, TX=IO4
    tft.fillScreen(displayColorBg());
    displayDrawHeaderBar("UART Bridge");
    tft.setTextSize(1);
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(10, displayHeaderHeight() + 10);
    tft.println("IO5=RX, IO4=TX");
    tft.printf("%d %d%c%c\n", uartConfig.baud, uartConfig.dataBits,
               uartConfig.parity == 0 ? 'N' : (uartConfig.parity == 1 ? 'O' : 'E'),
               uartConfig.stopBits == 1 ? '1' : '2');
    tft.println("Bridging USB<->target.");
    displayDrawFooterBar("BACK to stop");
}

void gpioHandleUARTActiveEvent(int evt) {
    (void)evt;
}

void gpioUARTBridgeTick() {
    if (uartBridgeActive) {
        UART_SERVICE.bridge(Serial, Serial);
    }
    if (terminalActive) {
        while (UART_SERVICE.available()) {
            char c = UART_SERVICE.read();
            if (c == '\n' || c == '\r') {
                terminalLines[terminalLineIndex] = terminalLog;
                terminalLog = "";
                terminalLineIndex = (terminalLineIndex + 1) % MAX_TERMINAL_LINES;
                drawTerminalScreen();
            } else {
                terminalLog += c;
                if (terminalLog.length() > 40)
                    terminalLog = terminalLog.substring(terminalLog.length() - 40);
            }
        }
    }
}

void gpioUartBridgeStop() {
    uartBridgeActive = false;
    terminalActive = false;
    UART_SERVICE.end();
}

static void drawTerminalScreen() {
    tft.fillScreen(displayColorBg());
    displayDrawHeaderBar("UART Terminal");
    tft.setTextSize(1);
    tft.setTextColor(displayColorFg(), displayColorBg());
    int y = displayHeaderHeight() + 4;
    for (int i = 0; i < MAX_TERMINAL_LINES; i++) {
        int idx = (terminalLineIndex + i) % MAX_TERMINAL_LINES;
        tft.setCursor(4, y);
        tft.print(terminalLines[idx]);
        y += 10;
    }
    // FIX: was "Type: keyboard  BACK: exit" — there is no on-screen
    // keyboard wired into the terminal, so that hint was misleading.
    displayDrawFooterBar("BACK: exit");
}

void uartEnterTerminal() {
    terminalActive = true;
    terminalLog = "";
    for (int i = 0; i < MAX_TERMINAL_LINES; i++) terminalLines[i] = "";
    terminalLineIndex = 0;
    UART_SERVICE.begin(PIN_GPIO_IO5, PIN_GPIO_IO4, uartConfig);   // RX=IO5, TX=IO4
    drawTerminalScreen();
}

void uartEnterSettings() {
    OnScreenKeyboardConfig cfg;
    osKeyboardUseStandardLayout(cfg);
    cfg.title = "UART Settings";
    cfg.subtitle = "Baud rate";
    cfg.maxLen = 7;
    cfg.okLabel = "Set";
    cfg.requireNonEmpty = true;
    String baudStr;
    osKeyboardEnter(cfg, baudStr);
    if (!osKeyboardWasCancelled()) {
        uint32_t b = strtol(baudStr.c_str(), nullptr, 10);
        if (b >= 300 && b <= 2000000) uartConfig.baud = b;
    }
    cfg.subtitle = "Parity (0=none,1=odd,2=even)";
    cfg.maxLen = 1;
    String parStr;
    osKeyboardEnter(cfg, parStr);
    if (!osKeyboardWasCancelled()) {
        uint8_t p = strtol(parStr.c_str(), nullptr, 10);
        if (p <= 2) uartConfig.parity = p;
    }
    cfg.subtitle = "Stop bits (1 or 2)";
    cfg.maxLen = 1;
    String stopStr;
    osKeyboardEnter(cfg, stopStr);
    if (!osKeyboardWasCancelled()) {
        uint8_t s = strtol(stopStr.c_str(), nullptr, 10);
        if (s == 1 || s == 2) uartConfig.stopBits = s;
    }
    displayShowMessage("UART", "Settings saved.");
    delay(800);
    enterState(STATE_GPIO_UART_MENU);
}

String uartSendCommand(const String& cmd, unsigned long timeoutMs) {
    bool wasActive = uartBridgeActive;
    if (!wasActive) {
        UART_SERVICE.begin(PIN_GPIO_IO5, PIN_GPIO_IO4, uartConfig);
    }
    UART_SERVICE.clearBuffer();
    UART_SERVICE.write((const uint8_t*)cmd.c_str(), cmd.length());
    UART_SERVICE.write('\r');
    UART_SERVICE.write('\n');
    String response = UART_SERVICE.readLine(timeoutMs);
    if (!wasActive) UART_SERVICE.end();
    return response;
}