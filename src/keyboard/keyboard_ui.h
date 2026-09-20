#pragma once
#include <Arduino.h>
#include <vector>

struct OnScreenKeyboardConfig {
    String title = "Enter Text";
    String subtitle = "";
    int maxLen = 20;
    String backLabel = "Cancel";
    String middleLabel = "Special";
    String okLabel = "OK";
    bool enableShuffle = false;
    const char** shuffleNames = nullptr;
    int shuffleCount = 0;
    bool requireNonEmpty = false;
    String emptyErrorMsg = "Text cannot be empty";
};

struct OnScreenKeyboardResult {
    String text;
    bool cancelled;
};

void osKeyboardUseStandardLayout(OnScreenKeyboardConfig& cfg);
void osKeyboardEnter(const OnScreenKeyboardConfig& cfg, String& output);
void osKeyboardTick();
bool osKeyboardHandleEvent(int evt, OnScreenKeyboardResult& result);
bool osKeyboardWasCancelled();   // <-- NEW