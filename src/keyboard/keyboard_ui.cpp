#include "keyboard_ui.h"
#include "display.h"
#include "buttons.h"
#include "config.h"

// -------------------------------------------------------------------
//  Internal state
// -------------------------------------------------------------------
static bool keyboardActive = false;
static bool keyboardCancelled = false;
static OnScreenKeyboardConfig currentConfig;
static String currentText = "";
static int cursorPos = 0;
static int selectedRow = 0;
static int selectedCol = 0;

// -------------------------------------------------------------------
//  Keyboard layout (standard QWERTY)
//
//  Bottom row: positions 0-5 are the special keys (SPACE, BACKSPACE,
//  CANCEL, OK, SHIFT, NUM), positions 6-8 are the printable chars.
//  The 3 printable slots hold '.', '-', ':' — useful punctuation that
//  hex/decimal input may need. The odd characters that used to sit in
//  those slots (',', '?', '!', '@') were unreachable behind the
//  specials anyway, so they've been dropped.
// -------------------------------------------------------------------
static const char* const KEYBOARD_ROWS[] = {
    "1234567890",
    "QWERTYUIOP",
    "ASDFGHJKL",
    "ZXCVBNM",
    "      .-:"     // 6 specials + 3 chars
};
static const int NUM_ROWS = 5;
static const int ROW_LENGTHS[] = {10, 10, 9, 7, 9};

// Special key identifiers
#define KEY_BACKSPACE  -1
#define KEY_SPACE      -2
#define KEY_CANCEL     -3
#define KEY_OK         -4
#define KEY_SHIFT      -5
#define KEY_NUM        -6

// -------------------------------------------------------------------
//  Helper: get character at (row, col)
// -------------------------------------------------------------------
static int getKeyCode(int row, int col) {
    if (row < 0 || row >= NUM_ROWS) return 0;
    const char* rowStr = KEYBOARD_ROWS[row];
    int len = ROW_LENGTHS[row];
    if (col < 0 || col >= len) return 0;
    if (row == 4 && col == 0) return KEY_SPACE;
    if (row == 4 && col == 1) return KEY_BACKSPACE;
    if (row == 4 && col == 2) return KEY_CANCEL;
    if (row == 4 && col == 3) return KEY_OK;
    if (row == 4 && col == 4) return KEY_SHIFT;
    if (row == 4 && col == 5) return KEY_NUM;
    return rowStr[col];
}

// -------------------------------------------------------------------
//  Drawing
// -------------------------------------------------------------------
static void drawKeyboard() {
    tft.fillScreen(displayColorBg());

    // Title bar
    displayDrawHeaderBar(currentConfig.title.c_str());

    // Subtitle
    tft.setTextSize(1);
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(6, displayHeaderHeight() + 2);
    tft.print(displayFitText(currentConfig.subtitle, tft.width() - 12, 1));

    // ---- Input text area ----
    const int boxX = 4;
    const int boxY = displayHeaderHeight() + 16;
    const int boxW = tft.width() - 8;
    const int boxH = 20;
    tft.fillRect(boxX, boxY, boxW, boxH, TFT_DARKGREY);
    tft.setTextColor(TFT_WHITE, TFT_DARKGREY);

    // Horizontal scroll: if the cursor has run past the visible area,
    // show a trailing window of the text so the cursor stays on screen.
    const int charW = 6;
    int maxVisible = (boxW - 8) / charW;
    int scroll = 0;
    if (cursorPos > maxVisible - 1) scroll = cursorPos - (maxVisible - 1);
    String visible = currentText.substring(scroll);
    tft.setCursor(boxX + 4, boxY + (boxH - 8) / 2);
    tft.print(displayFitText(visible, boxW - 8, 1));

    // Cursor
    int cursorScreenX = boxX + 4 + (cursorPos - scroll) * charW;
    if (cursorScreenX >= boxX + 2 && cursorScreenX <= boxX + boxW - 4) {
        tft.drawFastVLine(cursorScreenX, boxY + 3, boxH - 6, TFT_YELLOW);
    }

    // ---- Keyboard grid ----
    // Compute key width from the actual screen, not a hardcoded 260px.
    // The widest row has 10 keys, so that's what sets the width.
    const int cols    = 10;
    const int gap     = 2;                     // spacing between keys
    const int kbLeft  = 4;
    const int kbRight = tft.width() - 4;
    const int kbWidth = kbRight - kbLeft;

    int keyW = (kbWidth - (cols - 1) * gap) / cols;
    if (keyW < 12) keyW = 12;                  // safety floor

    int keyH = 22;
    int rowH = keyH + gap;

    int kbTop    = boxY + boxH + 6;
    int footerTop = tft.height() - displayFooterHeight();

    // If the grid would collide with the footer, shrink the key height
    // so it always fits vertically too.
    int kbHeight = NUM_ROWS * rowH - gap;
    if (kbTop + kbHeight > footerTop - 4) {
        int avail = footerTop - 4 - kbTop;
        keyH = (avail - (NUM_ROWS - 1) * gap) / NUM_ROWS;
        if (keyH < 14) keyH = 14;
        rowH = keyH + gap;
    }

    for (int row = 0; row < NUM_ROWS; row++) {
        int len = ROW_LENGTHS[row];
        int rowW = len * keyW + (len - 1) * gap;
        int rowX = kbLeft + (kbWidth - rowW) / 2;   // center short rows

        for (int col = 0; col < len; col++) {
            int x = rowX + col * (keyW + gap);
            int y = kbTop + row * rowH;
            bool sel = (row == selectedRow && col == selectedCol);
            int key = getKeyCode(row, col);

            uint16_t bg = sel ? displayColorSelectBg() : displayColorBg();
            uint16_t fg = sel ? displayColorSelectFg() : displayColorFg();

            tft.fillRoundRect(x, y, keyW, keyH, 3, bg);
            tft.drawRoundRect(x, y, keyW, keyH, 3, fg);

            tft.setTextColor(fg, bg);
            tft.setTextSize(1);

            // Build the label
            String label;
            if (key == KEY_BACKSPACE)         label = "<=";
            else if (key == KEY_SPACE)        label = "_";
            else if (key == KEY_CANCEL)       label = "X";
            else if (key == KEY_OK)           label = "OK";
            else if (key == KEY_SHIFT)        label = "^";
            else if (key == KEY_NUM)          label = "#";
            else if (key >= 32 && key <= 126) { char s[2] = {(char)key, 0}; label = s; }

            // Center label inside the key
            int textW = label.length() * 6;
            int textH = 8;
            int tx = x + (keyW - textW) / 2;
            int ty = y + (keyH - textH) / 2;
            tft.setCursor(tx, ty);
            tft.print(label);
        }
    }

    displayDrawFooterBar("OK=select  BACK=cancel");
}

// -------------------------------------------------------------------
//  Public functions
// -------------------------------------------------------------------
void osKeyboardUseStandardLayout(OnScreenKeyboardConfig& cfg) {
    // All fields already have defaults; this function exists so callers
    // can explicitly request the standard layout if they want to.
}

void osKeyboardEnter(const OnScreenKeyboardConfig& cfg, String& output) {
    keyboardActive = true;
    keyboardCancelled = false;
    currentConfig = cfg;
    currentText = "";
    cursorPos = 0;
    selectedRow = 0;
    selectedCol = 0;
    drawKeyboard();

    while (keyboardActive) {
        int evt = buttonsPoll();
        if (evt != EVT_NONE) {
            OnScreenKeyboardResult dummy;
            osKeyboardHandleEvent(evt, dummy);
        }
        osKeyboardTick();
        delay(10);
    }

    output = currentText;
}

void osKeyboardTick() { }

bool osKeyboardHandleEvent(int evt, OnScreenKeyboardResult& result) {
    if (!keyboardActive) return false;

    if (evt == EVT_BACK) {
        keyboardActive = false;
        keyboardCancelled = true;
        result.cancelled = true;
        return true;
    }

    if (evt == EVT_UP) {
        if (selectedRow > 0) {
            selectedRow--;
            int maxCol = ROW_LENGTHS[selectedRow] - 1;
            if (selectedCol > maxCol) selectedCol = maxCol;
            drawKeyboard();
        }
        return true;
    }
    if (evt == EVT_DOWN) {
        if (selectedRow < NUM_ROWS - 1) {
            selectedRow++;
            int maxCol = ROW_LENGTHS[selectedRow] - 1;
            if (selectedCol > maxCol) selectedCol = maxCol;
            drawKeyboard();
        }
        return true;
    }
    if (evt == EVT_LEFT) {
        if (selectedCol > 0) {
            selectedCol--;
            drawKeyboard();
        }
        return true;
    }
    if (evt == EVT_RIGHT) {
        int maxCol = ROW_LENGTHS[selectedRow] - 1;
        if (selectedCol < maxCol) {
            selectedCol++;
            drawKeyboard();
        }
        return true;
    }

    if (evt == EVT_OK) {
        int key = getKeyCode(selectedRow, selectedCol);
        if (key == KEY_CANCEL) {
            keyboardActive = false;
            keyboardCancelled = true;
            result.cancelled = true;
            return true;
        }
        if (key == KEY_OK) {
            if (currentConfig.requireNonEmpty && currentText.length() == 0) {
                displayShowMessage("Error", currentConfig.emptyErrorMsg.c_str());
                delay(500);
                drawKeyboard();
                return true;
            }
            keyboardActive = false;
            keyboardCancelled = false;
            result.cancelled = false;
            result.text = currentText;
            return true;
        }
        if (key == KEY_BACKSPACE) {
            if (cursorPos > 0) {
                String before = currentText.substring(0, cursorPos - 1);
                String after = currentText.substring(cursorPos);
                currentText = before + after;
                cursorPos--;
                drawKeyboard();
            }
            return true;
        }
        if (key == KEY_SPACE) {
            String before = currentText.substring(0, cursorPos);
            String after = currentText.substring(cursorPos);
            currentText = before + " " + after;
            cursorPos++;
            drawKeyboard();
            return true;
        }
        if (key == KEY_SHIFT) {
            // Not implemented — kept as a placeholder key
            return true;
        }
        if (key == KEY_NUM) {
            // Not implemented — kept as a placeholder key
            return true;
        }
        if (key >= 32 && key <= 126) {
            if ((int)currentText.length() < currentConfig.maxLen) {
                String before = currentText.substring(0, cursorPos);
                String after = currentText.substring(cursorPos);
                currentText = before + (char)key + after;
                cursorPos++;
                drawKeyboard();
            }
            return true;
        }
        return true;
    }

    return false;
}

bool osKeyboardWasCancelled() {
    return keyboardCancelled;
}