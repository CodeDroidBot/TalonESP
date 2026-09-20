#pragma once
#include <Arduino.h>

// ============================================================
//  3-Wire (93Cxx EEPROM) module – bit‑banged interface
//  Pins used: IO1=CLK, IO2=DATA, IO3=CS
//  Supports 93C46/56/66 (16‑bit organisation)
// ============================================================

// ---- Menu entry ----
void threewireEnterMenu();
void threewireHandleMenuEvent(int evt);

// ---- Actions ----
void threewireEnterRead();
void threewireEnterWrite();
void threewireEnterErase();

// ---- Write enable/disable (called internally) ----
void threewireWriteEnable();
void threewireWriteDisable();