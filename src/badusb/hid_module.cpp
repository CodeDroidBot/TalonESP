// hid_module.cpp
#include "hid_module.h"
#include "sdcard.h"
#include "config.h"
#include "display.h"
#include "buttons.h"
#include "menu.h"
#include "helpers.h"
#include <Adafruit_TinyUSB.h>
#include <SD.h>

// ============================================================
//  Global flag (defined here, extern in the header)
// ============================================================
bool g_scriptPickerMode = false;

// ============================================================
//  USB HID object (TinyUSB)
// ============================================================
static Adafruit_USBD_HID usb_hid;

// ============================================================
//  Mode state
// ============================================================
static HIDMode currentMode = HID_MODE_OFF;

// ============================================================
//  Current layout
// ============================================================
static KeyboardLayout currentLayout = KeyboardLayout::EN_US;

// ============================================================
//  Script execution state
// ============================================================
static bool    scriptRunning   = false;
static String  scriptStatus    = "Idle";
static int     scriptProgress  = 0;
static unsigned long delayUntil = 0;   // non-blocking DELAY

// ============================================================
//  Init
// ============================================================
void hidInit() {
    // Nothing to do until usbHidStart() is called.
}

void usbHidStart() {
    static const uint8_t desc_hid_report[] = {
        TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(1))
    };
    usb_hid.setPollInterval(2);
    usb_hid.setReportDescriptor(desc_hid_report, sizeof(desc_hid_report));
    usb_hid.begin();

    int attempts = 0;
    while (!usb_hid.ready() && attempts < 50) {
        delay(10);
        attempts++;
    }
    hidSetMode(HID_MODE_USB);
}

void usbHidStop() {
    hidSetMode(HID_MODE_OFF);
}

bool usbHidIsReady() {
    return usb_hid.ready();
}

// ============================================================
//  BLE HID stubs (disabled in this build)
// ============================================================
void bleHidStart(const char* deviceName) { (void)deviceName; }
void bleHidStop() { }
bool bleHidIsConnected() { return false; }

// ============================================================
//  Mode
// ============================================================
void hidSetMode(HIDMode mode)   { currentMode = mode; }
HIDMode hidGetMode()            { return currentMode; }

// ============================================================
//  Low-level HID operations
// ============================================================
void hidPressKey(uint8_t keyCode) {
    if (!usb_hid.ready()) return;
    uint8_t keycode[6] = {keyCode, 0, 0, 0, 0, 0};
    usb_hid.keyboardReport(1, 0, keycode);
    delay(5);
}

void hidReleaseKey(uint8_t keyCode) {
    (void)keyCode;
    if (!usb_hid.ready()) return;
    uint8_t empty[6] = {0, 0, 0, 0, 0, 0};
    usb_hid.keyboardReport(1, 0, empty);
    delay(5);
}

void hidPressModifierKey(uint8_t mod, uint8_t key) {
    if (!usb_hid.ready()) return;
    uint8_t keycode[6] = {key, 0, 0, 0, 0, 0};
    usb_hid.keyboardReport(1, mod, keycode);
    delay(5);
    uint8_t empty[6] = {0, 0, 0, 0, 0, 0};
    usb_hid.keyboardReport(1, 0, empty);
    delay(5);
}

void hidReleaseAll() {
    if (!usb_hid.ready()) return;
    uint8_t empty[6] = {0, 0, 0, 0, 0, 0};
    usb_hid.keyboardReport(1, 0, empty);
}

// ============================================================
//  Layout management
// ============================================================
void hidSetLayout(KeyboardLayout layout) { currentLayout = layout; }
KeyboardLayout hidGetLayout() { return currentLayout; }

// ============================================================
//  Text typing (layout-aware)
// ============================================================
bool hidTypeString(const String& text) {
    if (!usb_hid.ready()) return false;
    const uint8_t* keys = hidLayoutTable(currentLayout);
    for (size_t i = 0; i < text.length(); i++) {
        uint8_t ascii = (uint8_t)text[i];
        if (ascii >= 128) continue;
        uint8_t key = keys[ascii];
        if (key == 0) continue;
        uint8_t mod = hidLayoutModifierFor(currentLayout, ascii);
        if (mod) hidPressModifierKey(mod, key);
        else { hidPressKey(key); hidReleaseKey(key); }
        delay(2);
    }
    return true;
}

// ============================================================
//  Non-blocking delay hook (called from ducky_parser.cpp)
// ============================================================
void duckySetDelayUntil(unsigned long ms) { delayUntil = ms; }

// ============================================================
//  Script execution
// ============================================================
static std::vector<String> scriptLines;
static int scriptCurrentLine = 0;

bool hidRunScript(const String& path) {
    String content;
    if (!sdReadFile(path.c_str(), content)) {
        scriptStatus = "SD read failed";
        return false;
    }
    scriptLines.clear();
    int start = 0;
    while (start < (int)content.length()) {
        int nl = content.indexOf('\n', start);
        String line = (nl < 0) ? content.substring(start) : content.substring(start, nl);
        line.replace("\r", "");
        scriptLines.push_back(line);
        if (nl < 0) break;
        start = nl + 1;
    }
    Ducky::reset();
    Ducky::setLines(scriptLines);
    scriptCurrentLine = 0;
    scriptRunning = true;
    scriptStatus = "Running";
    scriptProgress = 0;
    delayUntil = 0;
    return true;
}

bool hidIsScriptRunning() { return scriptRunning; }

void hidStopScript() {
    scriptRunning = false;
    scriptStatus = "Stopped";
    hidReleaseAll();
}

void hidScriptTick() {
    if (!scriptRunning) return;
    if (millis() < delayUntil) return;

    if (scriptCurrentLine >= (int)scriptLines.size()) {
        scriptRunning = false;
        scriptStatus = "Done";
        hidReleaseAll();
        return;
    }

    String err;
    int rc = Ducky::executeLine(scriptLines[scriptCurrentLine], err);
    if (rc == 2) {
        scriptRunning = false;
        scriptStatus = "Error: " + err;
        hidReleaseAll();
        return;
    }
    scriptCurrentLine++;
    scriptProgress = (int)((scriptCurrentLine * 100) / max(1, (int)scriptLines.size()));
}

String hidScriptGetStatus()     { return scriptStatus; }
int    hidScriptGetProgress()   { return scriptProgress; }
int    hidScriptGetLineNumber() { return scriptCurrentLine; }
int    hidScriptGetTotalLines() { return (int)scriptLines.size(); }

// ============================================================
//  UI handlers
// ============================================================
static SimpleMenu hidActionMenu({"Type (Keyboard)", "Mouse Control", "Custom Script"});
static int pendingAction = 0;

void hidEnterMenu() {
    displayShowMenu("HID", hidActionMenu.items(), hidActionMenu.index());
}

void hidEnterModeSelect() {
    // Mode selection removed - always USB. If called, just start USB.
    usbHidStart();
    if (pendingAction == 0)      enterState(STATE_HID_KEYBOARD);
    else if (pendingAction == 1) enterState(STATE_HID_MOUSE);
    else                         enterState(STATE_HID_MENU);
}

void hidEnterKeyboard() {
    displayShowMessage("HID Keyboard", "OK = type 'Hello'\nBACK = exit");
}

void hidEnterMouse() {
    displayShowMessage("HID Mouse", "Mouse not implemented\nwith current libraries.");
}

void hidEnterScriptSelect() {
    g_scriptPickerMode = true;
    enterState(STATE_HID_SCRIPT_PICKER);
}

void hidEnterScriptRun() {
    displayShowMessage("DuckyScript", "Running... (BACK to stop)");
}

void hidMenuEvent(int evt) {
    if (evt == EVT_UP)   { hidActionMenu.up();   hidEnterMenu(); }
    if (evt == EVT_DOWN) { hidActionMenu.down(); hidEnterMenu(); }
    if (evt == EVT_OK) {
        pendingAction = hidActionMenu.index();
        if (pendingAction == 2) {
            hidEnterScriptSelect();
        } else {
            usbHidStart();
            if (pendingAction == 0)      enterState(STATE_HID_KEYBOARD);
            else if (pendingAction == 1) enterState(STATE_HID_MOUSE);
        }
    }
}

void hidModeSelectEvent(int evt) {
    (void)evt;   // Not used anymore
}

void hidKeyboardEvent(int evt) {
    if (evt == EVT_OK) {
        hidTypeString("Hello from GreyHat!\n");
        displayShowMessage("HID", "Typed 'Hello from GreyHat!'");
        delay(500);
        hidEnterKeyboard();
    }
}

void hidMouseEvent(int evt) {
    if (evt == EVT_OK) {
        displayShowMessage("HID", "Mouse not implemented\nwith current libraries.");
        delay(1000);
        hidEnterMouse();
    }
}

void hidScriptSelectEvent(int evt) { (void)evt; }

void hidScriptRunEvent(int evt) {
    if (evt == EVT_BACK) {
        hidStopScript();
        displayShowMessage("HID", "Script stopped");
        delay(500);
        hidSetMode(HID_MODE_OFF);
        enterState(STATE_HID_MENU);
    }
}