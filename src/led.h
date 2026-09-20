// led.h

#pragma once
#include <Arduino.h>

enum RfLedState {
    RF_LED_OFF,             // Sub-GHz module not active
    RF_LED_MENU,            // sitting in the Sub-GHz submenu (Read/Send/...)
    RF_LED_LISTENING,       // Read: waiting for a signal, fixed frequency
    RF_LED_HOPPING,         // Read: waiting for a signal, frequency hopping
    RF_LED_CAPTURED,        // Read: a valid signal is on screen
    RF_LED_SENDING,         // Read/Send: actively transmitting
    RF_LED_SAVED,           // Read: key just saved to SD (brief flash)
    RF_LED_RAW_ARMED,       // Read RAW: armed, waiting for edges
    RF_LED_RAW_RECORDING,   // Read RAW: actively capturing edges
    RF_LED_RAW_STOPPED,     // Read RAW: recording stopped, reviewing/saving
    RF_LED_TRANSMIT_BROWSE, // Send: browsing .sub files
    RF_LED_TRANSMIT_READY,  // Send: file loaded, waiting on OK to send
    RF_LED_ANALYZER,        // Analyzer screen active
    RF_LED_JAM_IDLE,        // Jammer screen open, not transmitting
    RF_LED_JAMMING,         // Jammer actively transmitting
    RF_LED_BRUTE_IDLE,      // Bruteforce intro screen, not running
    RF_LED_BRUTE_RUNNING,   // Bruteforce actively sending codes
    RF_LED_ERROR            // CC1101 not found / SD write fail / send failed
};

// Call once from setup (or from rfInit()) before the first rfLedSet().
void rfLedInit();

// Set the status LED to reflect the given RF state. Cheap to call often -
// safe to call every loop tick if convenient, it only re-writes the pixel
// when the state actually changes.
void rfLedSet(RfLedState state);