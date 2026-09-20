#include "buttons.h"
#include "config.h"
#include <Wire.h>
#include <Adafruit_MCP23X17.h>

static Adafruit_MCP23X17 mcp;

static const uint8_t BTN_PINS[6] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_OK, BTN_BACK};
static bool lastState[6]        = {true, true, true, true, true, true}; // HIGH = released (pull-up)
static unsigned long lastChange[6] = {0, 0, 0, 0, 0, 0};
static unsigned long debounceMs = 30;

void buttonsInit() {
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    Wire.setClock(100000);
    Wire.setTimeOut(50);

    if (!mcp.begin_I2C(0x20 + MCP23017_ADDR_OFFSET)) {
        delay(50);
        mcp.begin_I2C(0x20 + MCP23017_ADDR_OFFSET);
    }

    for (uint8_t i = 0; i < 6; i++) {
        mcp.pinMode(BTN_PINS[i], INPUT_PULLUP);
    }
}

void buttonsSetDebounceMs(unsigned long ms) { debounceMs = ms; }
unsigned long buttonsGetDebounceMs() { return debounceMs; }

int buttonsPoll() {
    unsigned long now = millis();

    for (uint8_t i = 0; i < 6; i++) {
        bool state = mcp.digitalRead(BTN_PINS[i]);
        if (state != lastState[i]) {
            delayMicroseconds(200);
            bool confirm = mcp.digitalRead(BTN_PINS[i]);
            if (confirm != state) {
                continue;   // glitch, ignore
            }
        }

        if (state != lastState[i] && (now - lastChange[i]) > debounceMs) {
            lastChange[i] = now;
            lastState[i]  = state;
            if (state == LOW) {
                return (int)i;
            }
        }
    }
    return EVT_NONE;
}