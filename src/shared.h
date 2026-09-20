#pragma once
#include <Arduino.h>

// Modules that need explicit power-down/stop when exiting their state.
enum PowerModule {
    POWER_MODULE_NONE = 0,
    POWER_MODULE_WIFI_PACKET_MON,
    POWER_MODULE_WIFI_DEAUTH,
    POWER_MODULE_WIFI_BEACON,
    POWER_MODULE_IR_TVBGONE,
    POWER_MODULE_IR_JAMMER,
    POWER_MODULE_IR,
    POWER_MODULE_UART_BRIDGE,
    POWER_MODULE_HID_SCRIPT,
    POWER_MODULE_HID_KEYBOARD,
    POWER_MODULE_HID_MOUSE,
    POWER_MODULE_SUBGHZ,
    POWER_MODULE_LORA,
    POWER_MODULE_GPIO_SPI,
    // Add more as needed
};

// Call once at boot (optional)
void powerInit();

// Stop a specific module (cleanly de-init / sleep)
void powerStopModule(PowerModule module);

// Stop all modules that are currently active (for emergency reset)
void powerStopAll();

// Helper: given an AppState, stop whatever modules that state was using.
// This is meant to be called when leaving a state (e.g., on BACK).
void powerExitState(int stateId);