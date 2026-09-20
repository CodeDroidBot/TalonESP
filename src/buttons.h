#pragma once
#include <Arduino.h>

enum ButtonEvent {
  EVT_NONE = -1,
  EVT_UP = 0,
  EVT_DOWN,
  EVT_LEFT,
  EVT_RIGHT,
  EVT_OK,
  EVT_BACK
};

void buttonsInit();

// Call every loop(). Returns a ButtonEvent once per press (debounced),
// or EVT_NONE if nothing changed.
int buttonsPoll();

// Runtime-adjustable debounce window, used by the Settings screen.
void buttonsSetDebounceMs(unsigned long ms);
unsigned long buttonsGetDebounceMs();