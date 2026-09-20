// rf_module.h
#pragma once
#include <Arduino.h>
#include <vector>


extern bool g_subPickerMode;

bool rfInit();
void rfProfilesInit();

void rfEnterMenu();
void rfHandleMenuEvent(int evt);

// Legacy stubs (not used)
void rfEnterSaved();
void rfHandleSavedEvent(int evt);
void rfEnterSavedAction();
void rfHandleSavedActionEvent(int evt);

// New Flipper .sub file support
bool rfPrepareSignalList(const char* path);
int rfGetSignalCount();
String rfGetSignalName(int index);
bool rfSendSelectedSignal(int index);

void rfJammerStart();
void rfJammerStop();
bool rfJammerIsRunning();

// Legacy replay (used by file action menu)
bool rfReplayFromSD(const char* path);

void rfRestoreReceiveMode();

// Internal state request (for main loop to open file picker)
extern int g_rfNextState;  // 0 = none, 1 = open file picker
void rfDeinit();