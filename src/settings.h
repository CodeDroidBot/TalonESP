#pragma once
#include <Arduino.h>

// Called once when entering the Settings screen (does not load from SD).
void settingsInit();

// Call every loop() iteration — flushes dirty changes to SD after a
// short debounce so rapid LEFT/RIGHT adjustments don't hammer the card.
void settingsTick();

// UP/DOWN = select row, LEFT/RIGHT = adjust. Any change marks the
// settings dirty; settingsTick() writes them to /settings.txt on SD.
void settingsHandleInput(int evt);

// Read /settings.txt from SD and apply every value. Silently falls back
// to defaults if the file is missing or SD isn't mounted.
void settingsLoad();

// Write current settings to /settings.txt on SD. No-op if SD is not
// mounted.
void settingsSave();