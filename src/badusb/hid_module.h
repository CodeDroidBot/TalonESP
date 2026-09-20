// hid_module.h
#pragma once
#include <Arduino.h>
#include <vector>

// ============================================================
//  Global flag for script picker mode (main.cpp toggles this)
// ============================================================
extern bool g_scriptPickerMode;

// ============================================================
//  HID transport mode
// ============================================================
enum HIDMode { HID_MODE_OFF, HID_MODE_BLE, HID_MODE_USB };
void    hidSetMode(HIDMode mode);
HIDMode hidGetMode();

// ============================================================
//  Keyboard layouts
// ============================================================
enum class KeyboardLayout : uint8_t {
    EN_US, DE_DE, FR_FR, ES_ES, IT_IT,
    PT_BR, PT_PT, SV_SE, DA_DK, HU_HU,
    COUNT
};

const char* hidLayoutName(KeyboardLayout layout);
KeyboardLayout hidLayoutFromString(const String& name);
const uint8_t* hidLayoutTable(KeyboardLayout layout);

// Returns the modifier byte for the given ASCII code in the given layout
// (0x00 = none, 0x02 = Left Shift, 0x40 = AltGr, etc.). Implemented in
// keyboard_layouts.cpp — declared here so both hid_module.cpp and
// ducky_parser.cpp resolve to the same global symbol.
uint8_t hidLayoutModifierFor(KeyboardLayout layout, uint8_t ascii);

// ============================================================
//  Core HID lifecycle
// ============================================================
void hidInit();
void usbHidStart();
void usbHidStop();
bool usbHidIsReady();

// BLE HID is disabled in this build - declared for compatibility.
void bleHidStart(const char* deviceName = "GreyHat");
void bleHidStop();
bool bleHidIsConnected();

// ============================================================
//  Low-level key operations
//  All take a USB HID usage code (not ASCII).
// ============================================================
void hidPressKey(uint8_t keyCode);
void hidReleaseKey(uint8_t keyCode);
void hidPressModifierKey(uint8_t mod, uint8_t key);
void hidReleaseAll();

// ============================================================
//  Layout-aware text typing
// ============================================================
void hidSetLayout(KeyboardLayout layout);
KeyboardLayout hidGetLayout();

// Type a string character by character. Returns false if USB HID
// isn't ready or a character couldn't be mapped.
bool hidTypeString(const String& text);

// ============================================================
//  DuckyScript engine
// ============================================================
bool hidRunScript(const String& path);      // load + execute /badusb/*.txt
bool hidIsScriptRunning();
void hidStopScript();                        // request stop
void hidScriptTick();                        // call every loop()

// Status for the UI
String hidScriptGetStatus();                 // "Idle", "Running", "Error: ..."
int    hidScriptGetProgress();               // 0-100
int    hidScriptGetLineNumber();
int    hidScriptGetTotalLines();

// ============================================================
//  DuckyScript parser API (implemented in ducky_parser.cpp)
// ============================================================
namespace Ducky {
    int  executeLine(const String& line, String& outError);
    void reset();
    void setLines(const std::vector<String>& lines);
    bool isRunning();
    int  lineNumber();
    int  totalLines();
}

// ============================================================
//  UI handlers  (invoked from main.cpp's state machine)
// ============================================================
void hidEnterMenu();
void hidEnterModeSelect();
void hidEnterKeyboard();
void hidEnterMouse();
void hidEnterScriptSelect();
void hidEnterScriptRun();

void hidMenuEvent(int evt);
void hidModeSelectEvent(int evt);
void hidKeyboardEvent(int evt);
void hidMouseEvent(int evt);
void hidScriptSelectEvent(int evt);
void hidScriptRunEvent(int evt);