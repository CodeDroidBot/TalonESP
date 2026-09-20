// ir_module.h
#pragma once
#include <Arduino.h>
#include <vector>

// ---- Global flag for IR picker mode ----
extern bool g_irPickerMode;   // true when the file manager should behave as an IR picker

// ======== Core IR functions ========
void irInit();
void irDeinit();
bool irReceiveLoop();
String irGetLastCodeSummary();
bool irSaveLastCodeToSD(const char* path);
bool irReplayFromSD(const char* path);
bool irReplayLastFromRAM();

uint8_t irGetRepeat();
void irSetRepeat(uint8_t n);
bool irGetAutoTx();
void irSetAutoTx(bool on);
uint32_t irGetAutoTxIntervalMs();
void irSetAutoTxIntervalMs(uint32_t ms);
void irAutoTxLoop();

bool irHasCapture();
const uint32_t* irGetLastRawUs();
uint16_t irGetLastRawLen();

int irUniversalCodeCount();
String irUniversalCodeLabel(int index);
void irSendUniversalCode(int index);

// Multi-signal .ir file support
int irOpenFileForPicking(const char* path);
String irOpenedFileSignalLabel(int index);
bool irSendOpenedFileSignal(int index);
String irLastSendError();

void irSendRawDemo();

// ======== UI entry points ========
void irEnterCaptureScreen();
void irHandleCaptureEvent(int evt);
void irEnterPlayFile();

// ======== Universal Remote (Flipper-style: category -> button grid -> spam-all) ========
void irEnterUniversalRemote();          // real entry point - resets state, call this, not irEnterUniversalCategory()
void irHandleUniversalEvent(int evt);
void irUniversalSendingLoop();          // MUST be called every loop() while the feature is active, or spam-sending never advances
bool irIsUniversalRemoteActive();       // true anywhere inside the feature (category list OR action grid OR sending)
bool irUniversalOnActionScreen();       // true only on the action grid (vs the category list) - use for BACK routing

// ======== Bruce-firmware-style IR features ========
void irEnterTvBGone();
void irHandleTvBGoneEvent(int evt);
void irTvBGoneLoop();
void irTvBGoneStop();

void irEnterJammer();
void irHandleJammerEvent(int evt);
void irJammerLoop();
void irJammerStop();

// ======== File Action Menu (Spam All / Choose Command / Back) ========
void irEnterFileActions();
void irHandleFileActionsEvent(int evt);
bool irIsInFileActions();
void irExitFileActions();   // exit the action menu and go back to file list

// ======== Spam All ========
void irSpamStart();
void irSpamStop();
void irSpamTick();          // call every loop()
bool irSpamIsRunning();
uint32_t irSpamGetCount();
uint32_t irSpamGetTotal();

// ======== Signal picker (for multi-signal .ir files) ========
void irEnterSignalPicker(const String& fileLabel, const std::vector<String>& labels);
void irHandleSignalPickerEvent(int evt);
void irSignalPickerBack();
bool irIsSignalPickerActive();

void irJammerStart();
void irJammerStop();
bool irJammerIsRunning();