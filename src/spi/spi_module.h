#pragma once

// ============================================================
//  SPI module — JEDEC ID probe today; Flash/EEPROM Dump and
//  Slave Mode are wired into the submenu as "(soon)" placeholders,
//  same as they were in gpio_module.cpp, just relocated here so
//  real implementations of those two can drop into this same
//  file later without touching gpio_module.cpp or main.cpp again.
//
//  This is the actual implementation behind gpio_module.h's
//  gpioEnterSPIMenu() / gpioHandleSPIMenuEvent() / gpioEnterSPIProbe() /
//  gpioHandleSPIProbeEvent() — gpio_module.cpp no longer defines them.
//
//  Wiring: IO1=SCK IO2=MOSI IO3=MISO IO4=CS, on the shared dedicated
//  bus from gpio_module.h's gpioGetSPI() (a dedicated FSPI instance) —
//  never the display/SD/CC1101 SPI bus.
// ============================================================

// Declared again here (identical signatures to gpio_module.h) so this
// folder is self-describing; gpio_module.h is still the header main.cpp
// actually includes to reach these.
void gpioEnterSPIMenu();
void gpioHandleSPIMenuEvent(int evt);
void gpioEnterSPIProbe();
void gpioHandleSPIProbeEvent(int evt);
