// main_helpers.h
#pragma once

// Global state‑transition function – takes an integer state ID.
// The actual AppState enum is defined only in main.cpp.
void enterState(int stateId);