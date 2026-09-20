#include "settings.h"
#include "display.h"
#include "buttons.h"
#include "sdcard.h"

// -------------------------------------------------------------------
//  Screen layout
// -------------------------------------------------------------------
static const int NUM_ROWS = 6;   // Brightness, Debounce, Theme, Mode, Orientation, Timeout
static int selRow = 0;

static const char* orientationLabels[4] = {"0°", "90°", "180°", "270°"};

// Timeout options
static const int TIMEOUT_OPTIONS[] = {0, 30, 60, 300, 600, 1800};
static const char* TIMEOUT_LABELS[] = {"Never", "30s", "1m", "5m", "10m", "30m"};
static const int TIMEOUT_COUNT = 6;

static int timeoutIndex = 0;

// -------------------------------------------------------------------
//  Persistence state
// -------------------------------------------------------------------
static const char* SETTINGS_PATH = "/settings.txt";

static bool          s_dirty        = false;
static unsigned long s_lastDirtyMs  = 0;
static const unsigned long SAVE_DEBOUNCE_MS = 400;

// -------------------------------------------------------------------
//  Save / Load
// -------------------------------------------------------------------
void settingsSave() {
    if (!sdIsMounted()) return;

    String out;
    out.reserve(192);
    out += "# GreyHat settings — auto-generated\n";
    out += "brightness=" + String(displayGetBacklight()) + "\n";
    out += "debounce="   + String((unsigned long)buttonsGetDebounceMs()) + "\n";
    out += "theme="      + String((int)displayGetThemeStyle()) + "\n";
    out += "mode="       + String((int)displayGetThemeMode())  + "\n";
    out += "rotation="   + String(displayGetRotation()) + "\n";
    out += "timeout="    + String(displayGetBacklightTimeout()) + "\n";

    sdWriteFile(SETTINGS_PATH, out);
    Serial.printf("[Settings] Saved to %s\n", SETTINGS_PATH);
}

void settingsLoad() {
    if (!sdIsMounted()) return;

    String content;
    if (!sdReadFile(SETTINGS_PATH, content) || content.length() == 0) {
        Serial.println("[Settings] No saved settings — using defaults");
        return;
    }

    int start = 0;
    int len = content.length();
    while (start < len) {
        int nl = content.indexOf('\n', start);
        String line = (nl >= 0) ? content.substring(start, nl) : content.substring(start);
        start = (nl >= 0) ? nl + 1 : len;
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) continue;

        int eq = line.indexOf('=');
        if (eq <= 0) continue;

        String key = line.substring(0, eq);
        String val = line.substring(eq + 1);
        key.trim();
        val.trim();
        key.toLowerCase();

        if (key == "brightness") {
            int v = constrain(val.toInt(), 16, 255);
            displaySetBacklight((uint8_t)v);
        } else if (key == "debounce") {
            long v = constrain(val.toInt(), 5L, 100L);
            buttonsSetDebounceMs((unsigned long)v);
        } else if (key == "theme") {
            // Defer theme application until after all lines are parsed
            // (we need both `theme` and `mode` before calling the setter)
        } else if (key == "mode") {
            // Deferred, see below
        } else if (key == "rotation") {
            int v = constrain(val.toInt(), 0, 3);
            displaySetRotation((uint8_t)v);
        } else if (key == "timeout") {
            int v = val.toInt();
            // Snap to nearest valid option
            int best = 0, bestDiff = 1 << 30;
            for (int i = 0; i < TIMEOUT_COUNT; i++) {
                int d = abs(TIMEOUT_OPTIONS[i] - v);
                if (d < bestDiff) { bestDiff = d; best = i; }
            }
            displaySetBacklightTimeout((uint16_t)TIMEOUT_OPTIONS[best]);
            timeoutIndex = best;
        }
    }

    // Now apply theme+mode together (they need to be parsed first).
    // Re-parse for these two specifically — cheap, file is tiny.
    ThemeStyle style = displayGetThemeStyle();
    ThemeMode  mode  = displayGetThemeMode();

    start = 0;
    while (start < len) {
        int nl = content.indexOf('\n', start);
        String line = (nl >= 0) ? content.substring(start, nl) : content.substring(start);
        start = (nl >= 0) ? nl + 1 : len;
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) continue;
        int eq = line.indexOf('=');
        if (eq <= 0) continue;
        String key = line.substring(0, eq);
        String val = line.substring(eq + 1);
        key.trim(); val.trim(); key.toLowerCase();
        if (key == "theme") {
            int t = val.toInt();
            if (t >= 0 && t < (int)ThemeStyle::Count) style = (ThemeStyle)t;
        } else if (key == "mode") {
            int m = val.toInt();
            if (m >= 0 && m < (int)ThemeMode::Count) mode = (ThemeMode)m;
        }
    }
    displaySetTheme(style, mode);

    Serial.println("[Settings] Loaded from SD");
}

// -------------------------------------------------------------------
//  Drawing
// -------------------------------------------------------------------
static void draw() {
    tft.fillScreen(displayColorBg());
    displayDrawHeaderBar("Settings");

    const int margin = displayMargin();
    const int rowH   = displayRowHeight();
    int y = displayHeaderHeight() + margin;

    int brightness = displayGetBacklight();
    unsigned long debounce = buttonsGetDebounceMs();
    uint16_t timeout = displayGetBacklightTimeout();

    // Sync timeoutIndex with current value
    timeoutIndex = 0;
    for (int i = 0; i < TIMEOUT_COUNT; i++) {
        if (TIMEOUT_OPTIONS[i] == timeout) { timeoutIndex = i; break; }
    }

    int rowW = tft.width() - 2 * margin;

    for (int row = 0; row < NUM_ROWS; row++) {
        bool sel = (row == selRow);
        String label, value;
        switch (row) {
            case 0: label = "Brightness"; value = String(brightness);                 break;
            case 1: label = "Debounce";   value = String((int)debounce) + "ms";       break;
            case 2: label = "Theme";      value = displayGetThemeName();              break;
            case 3: label = "Mode";       value = displayGetModeName();               break;
            case 4: {
                uint8_t rot = displayGetRotation();
                if (rot < 4) value = orientationLabels[rot];
                else value = "?";
                label = "Orientation";
                break;
            }
            case 5: label = "Timeout";    value = TIMEOUT_LABELS[timeoutIndex];       break;
        }
        drawListItem(margin, y, rowW, rowH, label, value, sel, true);
        y += rowH;
    }

    displayDrawFooterBar("UP/DN select   LEFT/RIGHT adjust");
}

// -------------------------------------------------------------------
//  Public entry / tick / input
// -------------------------------------------------------------------
void settingsInit() {
    selRow = 0;
    draw();
}

void settingsTick() {
    // Flush pending save after a short quiet period so rapid LEFT/RIGHT
    // adjustments don't write the SD card on every single step.
    if (s_dirty && (millis() - s_lastDirtyMs) >= SAVE_DEBOUNCE_MS) {
        settingsSave();
        s_dirty = false;
    }
}

void settingsHandleInput(int evt) {
    bool needRedraw = false;
    bool changed    = false;

    if (evt == EVT_UP) {
        selRow = (selRow > 0) ? selRow - 1 : NUM_ROWS - 1;
        needRedraw = true;
    } else if (evt == EVT_DOWN) {
        selRow = (selRow < NUM_ROWS - 1) ? selRow + 1 : 0;
        needRedraw = true;
    }

    if (evt == EVT_LEFT || evt == EVT_RIGHT) {
        int dir = (evt == EVT_RIGHT) ? 1 : -1;
        switch (selRow) {
            case 0: {
                int v = (int)displayGetBacklight() + dir * 16;
                v = constrain(v, 16, 255);
                displaySetBacklight((uint8_t)v);
                needRedraw = true;
                changed    = true;
                break;
            }
            case 1: {
                long v = (long)buttonsGetDebounceMs() + dir * 5;
                v = constrain(v, 5L, 100L);
                buttonsSetDebounceMs((unsigned long)v);
                needRedraw = true;
                changed    = true;
                break;
            }
            case 2:
                displayCycleTheme();
                needRedraw = true;
                changed    = true;
                break;
            case 3:
                displayToggleMode();
                needRedraw = true;
                changed    = true;
                break;
            case 4: {
                uint8_t rot = displayGetRotation();
                rot = (rot + dir) % 4;
                displaySetRotation(rot);
                draw();          // full redraw because layout changes
                needRedraw = false;
                changed    = true;
                break;
            }
            case 5: {
                timeoutIndex = (timeoutIndex + dir + TIMEOUT_COUNT) % TIMEOUT_COUNT;
                displaySetBacklightTimeout((uint16_t)TIMEOUT_OPTIONS[timeoutIndex]);
                needRedraw = true;
                changed    = true;
                break;
            }
        }
    }

    if (needRedraw) draw();

    if (changed) {
        s_dirty       = true;
        s_lastDirtyMs = millis();
    }
}