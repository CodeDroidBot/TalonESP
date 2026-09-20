#pragma once

// ============================================================
//  I2C module — address bus scan today; Slave Mode and EEPROM
//  Read are wired into the submenu as "(soon)" placeholders,
//  same as they were in gpio_module.cpp, just relocated here so
//  real implementations of those two can drop into this same
//  file later without touching gpio_module.cpp or main.cpp again.
//
//  This is the actual implementation behind gpio_module.h's
//  gpioEnterI2CMenu() / gpioHandleI2CMenuEvent() / gpioEnterI2CScan() /
//  gpioHandleI2CScanEvent() — gpio_module.cpp no longer defines them.
//
//  Wiring: IO1=SDA IO2=SCL, on the shared dedicated bus from
//  gpio_module.h's gpioGetWire() (Wire1) — never the buttons'/
//  MCP23017's default Wire.
// ============================================================

// Declared again here (identical signatures to gpio_module.h) so this
// folder is self-describing; gpio_module.h is still the header main.cpp
// actually includes to reach these.
void gpioEnterI2CMenu();
void gpioHandleI2CMenuEvent(int evt);
void gpioEnterI2CScan();
void gpioHandleI2CScanEvent(int evt);
