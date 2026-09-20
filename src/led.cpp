// led.cpp
#include "led.h"
#include "config.h"
#include <Adafruit_NeoPixel.h>

static Adafruit_NeoPixel statusPixel(NEOPIXEL_COUNT, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
static RfLedState currentState = RF_LED_OFF;
static bool initialized = false;

// Deliberately dim - this sits on a board with a TFT, SD card and a CC1101
// all sharing 5V/3V3, and it's a status glance, not a flashlight.
static const uint8_t LED_BRIGHTNESS = 40;

static inline void showColor(uint8_t r, uint8_t g, uint8_t b) {
    statusPixel.setPixelColor(0, statusPixel.Color(r, g, b));
    statusPixel.show();
}

void rfLedInit() {
    statusPixel.begin();
    statusPixel.setBrightness(LED_BRIGHTNESS);
    showColor(0, 0, 0);
    currentState = RF_LED_OFF;
    initialized = true;
}

void rfLedSet(RfLedState state) {
    if (!initialized) rfLedInit();
    if (state == currentState) return; // avoid redundant SPI/bit-bang writes
    currentState = state;

    switch (state) {
        case RF_LED_OFF:             showColor(0,   0,   0);   break;
        case RF_LED_MENU:            showColor(20,  20,  20);  break; // dim white
        case RF_LED_LISTENING:       showColor(0,   255, 255); break; // cyan
        case RF_LED_HOPPING:         showColor(255, 200, 0);   break; // yellow
        case RF_LED_CAPTURED:        showColor(0,   255, 0);   break; // green
        case RF_LED_SENDING:         showColor(255, 100, 0);   break; // orange
        case RF_LED_SAVED:           showColor(0,   100, 255); break; // blue
        case RF_LED_RAW_ARMED:       showColor(120, 0,   255); break; // purple
        case RF_LED_RAW_RECORDING:   showColor(255, 0,   255); break; // magenta
        case RF_LED_RAW_STOPPED:     showColor(60,  0,   120); break; // dim purple
        case RF_LED_TRANSMIT_BROWSE: showColor(255, 0,   180); break; // pink
        case RF_LED_TRANSMIT_READY:  showColor(200, 0,   255); break; // violet
        case RF_LED_ANALYZER:        showColor(0,   255, 150); break; // teal/spring green
        case RF_LED_JAM_IDLE:        showColor(80,  0,   0);   break; // dim red
        case RF_LED_JAMMING:         showColor(255, 0,   0);   break; // red
        case RF_LED_BRUTE_IDLE:      showColor(40,  40,  40);  break; // dim white
        case RF_LED_BRUTE_RUNNING:   showColor(255, 255, 255); break; // white
        case RF_LED_ERROR:           showColor(255, 0,   0);   break; // red
    }
}