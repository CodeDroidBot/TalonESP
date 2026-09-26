#include "display.h"
#include "config.h"
#include "icons.h"
#include <math.h>

TFT_eSPI tft = TFT_eSPI();

// -------------------------------------------------------------------
//  Layout constants
// -------------------------------------------------------------------
static const int MARGIN    = 8;
static const int HEADER_H  = 24;
static const int FOOTER_H  = 16;
static const int ROW_H     = 26;

// -------------------------------------------------------------------
//  Backlight & timeout state
// -------------------------------------------------------------------
static uint8_t backlightLevel = 100;
static uint8_t savedBrightness = 255;
static uint16_t timeoutSeconds = 0;
static unsigned long lastActivity = 0;
static bool isDimmed = false;

#ifndef BACKLIGHT_ACTIVE_LOW
#define BACKLIGHT_ACTIVE_LOW 0
#endif

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
static void backlightSetup() { ledcAttach(PIN_TFT_BL, 5000, 8); }
static void backlightWrite(uint8_t duty) {
#if BACKLIGHT_ACTIVE_LOW
  duty = 255 - duty;
#endif
  ledcWrite(PIN_TFT_BL, duty);
}
#else
static const int BL_CHANNEL = 0;
static void backlightSetup() {
  ledcSetup(BL_CHANNEL, 5000, 8);
  ledcAttachPin(PIN_TFT_BL, BL_CHANNEL);
}
static void backlightWrite(uint8_t duty) {
#if BACKLIGHT_ACTIVE_LOW
  duty = 255 - duty;
#endif
  ledcWrite(BL_CHANNEL, duty);
}
#endif

static String lastHeaderTitle = "";
static String lastFooterHint  = "";

// -------------------------------------------------------------------
//  Theme palette (declared early — many helpers below use `pal`)
// -------------------------------------------------------------------
struct ThemePalette {
  uint16_t bg;
  uint16_t headerBg;
  uint16_t fg;
  uint16_t fgDim;
  uint16_t selectBg;
  uint16_t selectFg;
  uint16_t scanline;
};

static ThemeStyle currentStyle = ThemeStyle::OrangeBlack;
static ThemeMode  currentMode  = ThemeMode::Dark;
static ThemePalette pal;
static uint32_t themeVersion = 0;

// HSV helper
static uint16_t hsv565(uint16_t h, uint8_t s, uint8_t v) {
  h %= 360;
  uint8_t region = h / 60;
  uint8_t f = (uint8_t)(((uint32_t)(h % 60) * 255) / 60);
  uint8_t p = (uint8_t)((uint32_t)v * (255 - s) / 255);
  uint8_t q = (uint8_t)((uint32_t)v * (255 - ((uint32_t)s * f / 255)) / 255);
  uint8_t t = (uint8_t)((uint32_t)v * (255 - ((uint32_t)s * (255 - f) / 255)) / 255);
  uint8_t r, g, b;
  switch (region) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// -------------------------------------------------------------------
//  Header status indicators (battery + SD)
// -------------------------------------------------------------------
static bool    g_sdMounted       = false;
static uint8_t g_batteryPercent  = 100;
static bool    g_batteryCharging = false;

void displaySetSdMounted(bool mounted) { g_sdMounted = mounted; }
bool displayGetSdMounted()             { return g_sdMounted; }
void displaySetBatteryLevel(uint8_t percent, bool charging) {
    if (percent > 100) percent = 100;
    g_batteryPercent  = percent;
    g_batteryCharging = charging;
}

static void drawBatteryIcon(int x, int y) {
    const int w = 20;
    const int h = 10;
    uint16_t color = g_batteryCharging ? TFT_CYAN :
                     (g_batteryPercent > 50) ? TFT_GREEN :
                     (g_batteryPercent > 20) ? TFT_YELLOW : TFT_RED;

    tft.drawRect(x, y, w, h, color);
    tft.fillRect(x + w, y + 3, 2, h - 6, color);

    int fillW = (g_batteryPercent * (w - 4)) / 100;
    if (fillW > 0) tft.fillRect(x + 2, y + 2, fillW, h - 4, color);

    if (g_batteryCharging) {
        tft.drawFastVLine(x + w / 2,     y + 2, 3, pal.headerBg);
        tft.drawFastVLine(x + w / 2 + 1, y + 5, 3, pal.headerBg);
    }
}

static void drawSdIcon(int x, int y) {
    uint16_t color = g_sdMounted ? TFT_GREEN : TFT_RED;

    tft.drawLine(x + 3,  y,      x + 11, y,      color);
    tft.drawLine(x,      y + 3,  x + 3,  y,      color);
    tft.drawLine(x,      y + 3,  x,      y + 13, color);
    tft.drawLine(x,      y + 13, x + 11, y + 13, color);
    tft.drawLine(x + 11, y,      x + 11, y + 13, color);

    tft.drawPixel(x + 4, y + 2, color);
    tft.drawPixel(x + 6, y + 2, color);
    tft.drawPixel(x + 8, y + 2, color);

    if (g_sdMounted) {
        tft.drawLine(x + 3, y + 9,  x + 5, y + 11, color);
        tft.drawLine(x + 5, y + 11, x + 9, y + 6,  color);
    } else {
        tft.drawLine(x + 4, y + 7, x + 8, y + 11, color);
        tft.drawLine(x + 8, y + 7, x + 4, y + 11, color);
    }
}

static void drawHeaderIndicators() {
    int w = tft.width();
    int sdX  = w - 4 - 12;
    int batX = sdX - 4 - 20;
    int batY = (HEADER_H - 10) / 2;
    int sdY  = (HEADER_H - 14) / 2;

    drawSdIcon(sdX, sdY);
    drawBatteryIcon(batX, batY);
}

// -------------------------------------------------------------------
//  Thin outline around the whole display
// -------------------------------------------------------------------
void displayDrawBorder() {
    int w = tft.width();
    int h = tft.height();
    uint16_t c = pal.fgDim;
    tft.drawFastHLine(0, 0,     w, c);
    tft.drawFastHLine(0, h - 1, w, c);
    tft.drawFastVLine(0,     0, h, c);
    tft.drawFastVLine(w - 1, 0, h, c);
}

// -------------------------------------------------------------------
//  Theme palette builder + theme API
// -------------------------------------------------------------------
static ThemePalette buildPalette(ThemeStyle style, ThemeMode mode, uint16_t /*hue*/) {
  ThemePalette p{};
  switch (style) {
    case ThemeStyle::OrangeBlack:
      if (mode == ThemeMode::Dark) {
        p.bg        = TFT_BLACK;
        p.headerBg  = tft.color565(20, 20, 20);
        p.fg        = tft.color565(255, 140, 0);
        p.fgDim     = tft.color565(160, 90, 0);
        p.selectBg  = tft.color565(255, 140, 0);
        p.selectFg  = TFT_BLACK;
        p.scanline  = tft.color565(15, 15, 15);
      } else {
        p.bg        = tft.color565(245, 245, 245);
        p.headerBg  = tft.color565(220, 220, 220);
        p.fg        = tft.color565(200, 80, 0);
        p.fgDim     = tft.color565(180, 120, 80);
        p.selectBg  = tft.color565(200, 80, 0);
        p.selectFg  = tft.color565(245, 245, 245);
        p.scanline  = tft.color565(230, 230, 230);
      }
      break;

    case ThemeStyle::TerminalGreen:
      if (mode == ThemeMode::Dark) {
        p.bg        = TFT_BLACK;
        p.headerBg  = tft.color565(0, 30, 0);
        p.fg        = tft.color565(40, 255, 40);
        p.fgDim     = tft.color565(0, 110, 0);
        p.selectBg  = tft.color565(40, 255, 40);
        p.selectFg  = TFT_BLACK;
        p.scanline  = tft.color565(0, 10, 0);
      } else {
        p.bg        = tft.color565(225, 245, 225);
        p.headerBg  = tft.color565(170, 220, 170);
        p.fg        = tft.color565(0, 90, 0);
        p.fgDim     = tft.color565(90, 150, 90);
        p.selectBg  = tft.color565(0, 90, 0);
        p.selectFg  = tft.color565(225, 245, 225);
        p.scanline  = tft.color565(205, 230, 205);
      }
      break;

    case ThemeStyle::OrangeGrey:
      if (mode == ThemeMode::Dark) {
        p.bg        = tft.color565(24, 24, 24);
        p.headerBg  = tft.color565(58, 40, 12);
        p.fg        = tft.color565(255, 145, 0);
        p.fgDim     = tft.color565(140, 100, 50);
        p.selectBg  = tft.color565(255, 145, 0);
        p.selectFg  = tft.color565(24, 24, 24);
        p.scanline  = tft.color565(38, 36, 32);
      } else {
        p.bg        = tft.color565(238, 236, 230);
        p.headerBg  = tft.color565(214, 182, 148);
        p.fg        = tft.color565(150, 75, 0);
        p.fgDim     = tft.color565(160, 130, 100);
        p.selectBg  = tft.color565(150, 75, 0);
        p.selectFg  = tft.color565(238, 236, 230);
        p.scanline  = tft.color565(222, 216, 204);
      }
      break;

    default: break;
  }
  return p;
}

static void applyTheme() {
  pal = buildPalette(currentStyle, currentMode, 0);
  themeVersion++;
}

void displayCycleTheme() {
  int next = (static_cast<int>(currentStyle) + 1) % static_cast<int>(ThemeStyle::Count);
  currentStyle = static_cast<ThemeStyle>(next);
  applyTheme();
}

void displayToggleMode() {
  currentMode = (currentMode == ThemeMode::Dark) ? ThemeMode::Light : ThemeMode::Dark;
  applyTheme();
}

void displaySetTheme(ThemeStyle style, ThemeMode mode) {
    if (style >= ThemeStyle::Count) style = ThemeStyle::OrangeBlack;
    if (mode  >= ThemeMode::Count)  mode  = ThemeMode::Dark;
    currentStyle = style;
    currentMode  = mode;
    applyTheme();
}

ThemeStyle displayGetThemeStyle() { return currentStyle; }
ThemeMode  displayGetThemeMode()  { return currentMode; }

const char* displayGetThemeName() {
  switch (currentStyle) {
    case ThemeStyle::OrangeBlack:   return "Cifer Div";
    case ThemeStyle::TerminalGreen: return "Terminal Green";
    case ThemeStyle::OrangeGrey:    return "Orange/Grey";
    default:                        return "?";
  }
}

const char* displayGetModeName() {
  return (currentMode == ThemeMode::Dark) ? "Dark" : "Light";
}

// -------------------------------------------------------------------
//  Display rotation
// -------------------------------------------------------------------
void displaySetRotation(uint8_t rotation) { tft.setRotation(rotation); }
uint8_t displayGetRotation() { return tft.getRotation(); }

// -------------------------------------------------------------------
//  Display initialization
// -------------------------------------------------------------------
void displayInit() {
  tft.init();
  tft.setRotation(0);
  backlightSetup();
  displaySetBacklight(100);
  applyTheme();
  tft.fillScreen(pal.bg);
  tft.setTextColor(pal.fg, pal.bg);
  tft.setTextSize(1);
  lastActivity = millis();
  savedBrightness = 255;
  isDimmed = false;
}

// -------------------------------------------------------------------
//  Backlight control
// -------------------------------------------------------------------
void displaySetBacklight(uint8_t brightness) {
    savedBrightness = brightness;
    if (!isDimmed) backlightWrite(brightness);
}
void displaySetUserBrightness(uint8_t brightness) { displaySetBacklight(brightness); }
uint8_t displayGetBacklight() { return backlightLevel; }

void displaySetBacklightTimeout(uint16_t seconds) {
    timeoutSeconds = seconds;
    displayResetIdleTimer();
}
uint16_t displayGetBacklightTimeout() { return timeoutSeconds; }

void displayResetIdleTimer() {
    lastActivity = millis();
    if (isDimmed) {
        isDimmed = false;
        displaySetBacklight(savedBrightness);
    }
}

void displayCheckIdle() {
    if (timeoutSeconds == 0) return;
    if (isDimmed) return;
    unsigned long now = millis();
    if (now - lastActivity > (unsigned long)timeoutSeconds * 1000) {
        isDimmed = true;
        backlightWrite(10);
    }
}

bool displayIsDimmed() { return isDimmed; }

// -------------------------------------------------------------------
//  RGB API stubs (no-op)
// -------------------------------------------------------------------
void     displayTick()          { }
uint16_t displayGetRgbHue()     { return 0; }
void     displaySetRgbSpeed(uint8_t) { }
uint8_t  displayGetRgbSpeed()   { return 0; }

// -------------------------------------------------------------------
//  Boot splash — uses 128x128 icon from icons.h
//
//  Uses pushImage() which expects RGB565 (uint16_t) data.
//  If your icon_boot[] in icons.h is `unsigned char` (1-bit), see
//  the note below for the drawBitmap() alternative.
// -------------------------------------------------------------------
void displayShowBootSplash(const char* deviceName) {
    tft.fillScreen(pal.bg);
    displayDrawBorder();

    int screenW = tft.width();
    int screenH = tft.height();

    int iconX = (screenW - 180) / 2;
    int iconY = (screenH - 180) / 2 - 40;

    tft.drawBitmap(iconX, iconY, boot_logo, 180, 180, pal.fg);

    if (deviceName && deviceName[0]) {
        tft.setTextSize(2);
        int dw = strlen(deviceName) * 12;
        tft.setTextColor(pal.fg, pal.bg);
        tft.setCursor((screenW - dw) / 2, iconY + 180 + 20);
        tft.print(deviceName);
    }

#if defined(DEVICE_AUTHOR)
    const char* credit = DEVICE_AUTHOR;
    tft.setTextSize(1);
    int cw = strlen(credit) * 6;
    tft.setTextColor(pal.fgDim, pal.bg);
    tft.setCursor((screenW - cw) / 2, iconY + 180 + 48);
    tft.print(credit);
#endif
}
// -------------------------------------------------------------------
//  Drawing primitives
// -------------------------------------------------------------------
void displayClear() {
  tft.fillScreen(pal.bg);
  displayDrawBorder();
}

static void drawScanlines(int topY = 0) {
  for (int y = topY; y < tft.height(); y += 3) {
    tft.drawFastHLine(0, y, tft.width(), pal.scanline);
  }
}

String displayFitText(const String& text, int maxWidthPx, int textSize) {
  int charPx = 6 * textSize;
  if (charPx <= 0) return text;
  int maxChars = maxWidthPx / charPx;
  if (maxChars < 1) maxChars = 1;
  if ((int)text.length() <= maxChars) return text;
  if (maxChars <= 2) return text.substring(0, maxChars);
  return text.substring(0, maxChars - 2) + "..";
}

void displayDrawHeaderBar(const char* title) {
  lastHeaderTitle = title ? title : "";

  tft.fillRect(0, 0, tft.width(), HEADER_H, pal.headerBg);

  int reservedRight = 44;
  int titleAvailW = tft.width() - reservedRight;
  String fitted = displayFitText(String(title), titleAvailW - 2 * MARGIN, 2);
  int textW = fitted.length() * 12;
  int x = (titleAvailW - textW) / 2;
  if (x < MARGIN) x = MARGIN;

  tft.setTextColor(pal.fg, pal.headerBg);
  tft.setTextSize(2);
  tft.setCursor(x, (HEADER_H - 16) / 2);
  tft.print(fitted);

  drawHeaderIndicators();

  // Divider between header and content
  tft.drawFastHLine(0, HEADER_H,     tft.width(), pal.fg);
  tft.drawFastHLine(0, HEADER_H + 1, tft.width(), pal.scanline);

  // Outer border edges
  tft.drawFastHLine(0, 0, tft.width(), pal.fgDim);
  tft.drawFastVLine(0, 0, tft.height(), pal.fgDim);
  tft.drawFastVLine(tft.width() - 1, 0, tft.height(), pal.fgDim);
}

int displayHeaderHeight() { return HEADER_H + 2; }

void displayDrawFooterBar(const char* hint) {
  lastFooterHint = hint ? hint : "";
  int y = tft.height() - FOOTER_H;
  tft.fillRect(0, y, tft.width(), FOOTER_H, pal.bg);
  tft.drawFastHLine(0, y, tft.width(), pal.fgDim);
  tft.setTextColor(pal.fgDim, pal.bg);
  tft.setTextSize(1);
  String fitted = displayFitText(String(hint), tft.width() - 2 * MARGIN - 2, 1);
  tft.setCursor(MARGIN, y + (FOOTER_H - 8) / 2 + 1);
  tft.print(fitted);

  tft.drawFastHLine(0, tft.height() - 1, tft.width(), pal.fgDim);
}

int displayFooterHeight() { return FOOTER_H; }
int displayMargin()    { return MARGIN; }
int displayRowHeight() { return ROW_H; }

uint16_t displayColorBg()        { return pal.bg; }
uint16_t displayColorFg()        { return pal.fg; }
uint16_t displayColorFgDim()     { return pal.fgDim; }
uint16_t displayColorHeaderBg()  { return pal.headerBg; }
uint16_t displayColorSelectBg()  { return pal.selectBg; }
uint16_t displayColorSelectFg()  { return pal.selectFg; }

// -------------------------------------------------------------------
//  Menu / Message screens
// -------------------------------------------------------------------
static bool stringVectorsEqual(const std::vector<String>& a, const std::vector<String>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++) {
    if (!a[i].equals(b[i])) return false;
  }
  return true;
}

static int textMenuTopIndex = 0;
static String textMenuLastTitle;
static std::vector<String> textMenuLastItems;
static int textMenuLastSelected = -1;
static int textMenuLastTopIndex = -1;
static uint32_t textMenuLastThemeVersion = 0;

void displayShowMenu(const char* title, const std::vector<String>& items, int selectedIndex) {
  tft.setTextSize(2);
  const int gutterW = 18;
  int top = HEADER_H + 2 + MARGIN;
  int n = (int)items.size();

  int availH = tft.height() - top;
  int visibleRows = max(1, availH / ROW_H);
  bool scrolling = n > visibleRows;

  bool sameMenu = (title != nullptr) && textMenuLastTitle == title
                  && stringVectorsEqual(textMenuLastItems, items)
                  && textMenuLastThemeVersion == themeVersion;
  if (!sameMenu) textMenuTopIndex = 0;

  int maxTopIndex = max(0, n - visibleRows);
  if (selectedIndex < textMenuTopIndex) textMenuTopIndex = selectedIndex;
  if (selectedIndex >= textMenuTopIndex + visibleRows) textMenuTopIndex = selectedIndex - visibleRows + 1;
  if (textMenuTopIndex > maxTopIndex) textMenuTopIndex = maxTopIndex;
  if (textMenuTopIndex < 0) textMenuTopIndex = 0;

  int scrollbarW = scrolling ? 5 : 0;
  int labelMaxPx = tft.width() - MARGIN - gutterW - MARGIN - scrollbarW;

  auto drawRow = [&](int i, bool sel) {
    int y = top + (i - textMenuTopIndex) * ROW_H;
    uint16_t bg = sel ? pal.selectBg : pal.bg;
    uint16_t fg = sel ? pal.selectFg : pal.fgDim;

    if (sel) {
      tft.fillRoundRect(0, y, tft.width() - scrollbarW, ROW_H, 4, bg);
    } else {
      tft.fillRect(0, y, tft.width() - scrollbarW, ROW_H, bg);
    }

    tft.setTextColor(fg, bg);
    int textY = y + (ROW_H - 16) / 2;
    tft.setCursor(MARGIN, textY);
    tft.print(sel ? ">" : " ");
    tft.setCursor(MARGIN + gutterW, textY);
    tft.print(displayFitText(items[i], labelMaxPx, 2));
  };

  bool canPartialUpdate = sameMenu
      && textMenuLastTopIndex == textMenuTopIndex
      && textMenuLastSelected >= 0
      && textMenuLastSelected != selectedIndex
      && textMenuLastSelected >= textMenuTopIndex && textMenuLastSelected < textMenuTopIndex + visibleRows
      && selectedIndex >= textMenuTopIndex && selectedIndex < textMenuTopIndex + visibleRows;

  if (canPartialUpdate) {
    drawRow(textMenuLastSelected, false);
    drawRow(selectedIndex, true);
  } else {
    tft.fillScreen(pal.bg);
    drawScanlines(HEADER_H + 2);
    displayDrawHeaderBar(title);

    int lastVisible = min(n, textMenuTopIndex + visibleRows);
    for (int i = textMenuTopIndex; i < lastVisible; i++) {
      drawRow(i, i == selectedIndex);
    }

    if (scrolling) {
      int trackX = tft.width() - 3;
      int trackH = visibleRows * ROW_H;
      tft.drawRect(trackX, top, 3, trackH, pal.fgDim);
      int thumbH = max(8, trackH * visibleRows / n);
      int thumbY = top + (trackH - thumbH) * textMenuTopIndex / max(1, maxTopIndex);
      tft.fillRect(trackX, thumbY, 3, thumbH, pal.fg);
    }
    displayDrawBorder();
  }

  textMenuLastTitle = title ? title : "";
  textMenuLastItems = items;
  textMenuLastSelected = selectedIndex;
  textMenuLastTopIndex = textMenuTopIndex;
  textMenuLastThemeVersion = themeVersion;
}

void displayShowMessage(const char* title, const char* msg) {
  tft.fillScreen(pal.bg);
  displayDrawHeaderBar(title);

  tft.setTextColor(pal.fg, pal.bg);
  tft.setTextSize(1);
  tft.setCursor(8, HEADER_H + 14);
  tft.print(msg);

  displayDrawBorder();
}

void drawProgressBar(int x, int y, int w, int h, float progress, uint16_t barColor, uint16_t bgColor) {
  if (progress < 0.0f) progress = 0.0f;
  if (progress > 1.0f) progress = 1.0f;
  int fill = (int)(w * progress);
  tft.fillRect(x, y, w, h, bgColor);
  if (fill > 0) tft.fillRect(x, y, fill, h, barColor);
  tft.drawRect(x, y, w, h, displayColorFgDim());
}

// -------------------------------------------------------------------
//  drawListItem (used in Settings etc.)
// -------------------------------------------------------------------
void drawListItem(int x, int y, int w, int h, const String& label,
                   const String& value, bool selected, bool hasValue) {
  uint16_t bg = selected ? pal.selectBg : pal.bg;
  uint16_t fg = selected ? pal.selectFg : pal.fg;
  uint16_t valueFg = selected ? pal.selectFg : pal.fgDim;

  if (selected) {
    tft.fillRoundRect(x, y, w, h, 4, bg);
  } else {
    tft.fillRect(x, y, w, h, bg);
  }

  tft.setTextSize(1);

  int textY = y + (h - 8) / 2;
  int valueMaxPx = w / 2;
  String fittedValue = (hasValue && value.length() > 0)
                            ? displayFitText(value, valueMaxPx, 1)
                            : String("");
  int labelMaxPx = w - MARGIN - MARGIN - (fittedValue.length() ? (int)fittedValue.length() * 6 + MARGIN : 0);

  tft.setTextColor(fg, bg);
  tft.setCursor(x + MARGIN, textY);
  tft.print(displayFitText(label, labelMaxPx, 1));

  if (fittedValue.length() > 0) {
    int valueW = fittedValue.length() * 6;
    tft.setTextColor(valueFg, bg);
    tft.setCursor(x + w - MARGIN - valueW, textY);
    tft.print(fittedValue);
  }
}

// -------------------------------------------------------------------
//  Icon grid (home screen)
// -------------------------------------------------------------------
static void drawVectorIcon(MenuIcon icon, int cx, int cy, uint16_t color) {
    switch (icon) {
        case MenuIcon::Deauth: {
            tft.drawCircle(cx, cy, 16, color);
            tft.drawLine(cx - 13, cy - 13, cx + 13, cy + 13, color);
            tft.drawLine(cx - 13, cy - 12, cx + 13, cy + 14, color);
            break;
        }
        case MenuIcon::Beacon: {
            tft.fillCircle(cx, cy + 9, 4, color);
            tft.drawCircle(cx, cy + 9, 11, color);
            tft.drawCircle(cx, cy + 9, 17, color);
            break;
        }
        case MenuIcon::Script: {
            tft.drawRoundRect(cx - 13, cy - 15, 26, 30, 3, color);
            tft.drawFastHLine(cx - 8, cy - 7, 12, color);
            tft.drawFastHLine(cx - 8, cy - 1, 14, color);
            tft.drawFastHLine(cx - 8, cy + 5, 10, color);
            break;
        }
        case MenuIcon::Layout: {
            const int s = 12, g = 2;
            tft.fillRect(cx - g/2 - s, cy - g/2 - s, s, s, color);
            tft.fillRect(cx + g/2,     cy - g/2 - s, s, s, color);
            tft.fillRect(cx - g/2 - s, cy + g/2,     s, s, color);
            tft.fillRect(cx + g/2,     cy + g/2,     s, s, color);
            break;
        }
        default: break;
    }
}

static void drawIconBitmap(MenuIcon icon, int cx, int cy, uint16_t color, float scale) {
    const unsigned char* bitmap = nullptr;
    switch (icon) {
        case MenuIcon::SubGhz:   bitmap = icon_subghz; break;
        case MenuIcon::Infrared: bitmap = icon_infrared; break;
        case MenuIcon::Files:    bitmap = icon_files; break;
        case MenuIcon::Wifi:     bitmap = icon_wifi; break;
        case MenuIcon::Ble:      bitmap = icon_ble; break;
        case MenuIcon::UsbHid:   bitmap = icon_usbhid; break;
        case MenuIcon::Games:    bitmap = icon_games; break;
        case MenuIcon::Settings: bitmap = icon_settings; break;
        case MenuIcon::About:    bitmap = icon_about; break;
        case MenuIcon::Gps:      bitmap = icon_gps; break;
        case MenuIcon::Lora:     bitmap = icon_lora; break;
        case MenuIcon::Deauth:
        case MenuIcon::Beacon:
        case MenuIcon::Script:
        case MenuIcon::Layout:
            drawVectorIcon(icon, cx, cy, color);
            return;
        default: return;
    }
    tft.drawBitmap(cx - 17, cy - 17, bitmap, 32, 32, color);
}

static bool iconVectorsEqual(const std::vector<MenuIcon>& a, const std::vector<MenuIcon>& b) {
  return a == b;
}

static int iconMenuTopRow = 0;
static String iconMenuLastTitle;
static std::vector<MenuIcon> iconMenuLastIcons;
static std::vector<String> iconMenuLastLabels;
static int iconMenuLastSelected = -1;
static int iconMenuLastTopRow = -1;
static uint32_t iconMenuLastThemeVersion = 0;

void displayShowIconMenu(const char* title, const std::vector<MenuIcon>& icons,
                          const std::vector<String>& labels, int selectedIndex) {
  const int cols = 2;
  int n = (int)icons.size();
  int totalRows = (n + cols - 1) / cols;
  int top = HEADER_H + 2;
  int bottom = tft.height() - MARGIN;
  int availH = bottom - top;

  const int cellMargin = 6;
  int labelTextSize = 2;
  int labelCharW = 6 * labelTextSize;
  int lineH = 8 * labelTextSize;
  int labelPad = 4;
  int labelAreaH = lineH + labelPad;
  int iconAreaH = 38;
  int cellH = iconAreaH + labelAreaH + 2 * cellMargin;

  int visibleRows = max(1, availH / cellH);
  bool scrolling = totalRows > visibleRows;
  int maxTopRow = max(0, totalRows - visibleRows);

  bool sameMenu = (title != nullptr) && iconMenuLastTitle == title
                  && iconVectorsEqual(iconMenuLastIcons, icons)
                  && stringVectorsEqual(iconMenuLastLabels, labels)
                  && iconMenuLastThemeVersion == themeVersion;
  if (!sameMenu) iconMenuTopRow = 0;

  int selRow = selectedIndex / cols;
  if (selRow < iconMenuTopRow) iconMenuTopRow = selRow;
  if (selRow >= iconMenuTopRow + visibleRows) iconMenuTopRow = selRow - visibleRows + 1;
  if (iconMenuTopRow > maxTopRow) iconMenuTopRow = maxTopRow;
  if (iconMenuTopRow < 0) iconMenuTopRow = 0;

  int gridW = tft.width() - (scrolling ? 5 : 0);
  int cellW = gridW / cols;
  int innerW = cellW - 2 * cellMargin;
  int innerH = cellH - 2 * cellMargin;
  int maxLabelChars = max(1, innerW / labelCharW);

  int shownRows = min(visibleRows, totalRows - iconMenuTopRow);
  int contentH = shownRows * cellH;
  int gridTop = top + max(0, (availH - contentH) / 2);

  auto cellRect = [&](int i, int& x, int& y) {
    int col = i % cols;
    int row = i / cols;
    x = col * cellW;
    y = gridTop + (row - iconMenuTopRow) * cellH;
  };

  auto drawCell = [&](int i, bool sel) {
    int x, y;
    cellRect(i, x, y);
    uint16_t c = sel ? pal.selectFg : pal.fgDim;
    int cx = x + cellW / 2;
    int iconCy = y + cellMargin + iconAreaH / 2;

    if (sel) {
      tft.fillRoundRect(x + 2, y + 2, cellW - 4, cellH - 4, 5, pal.selectBg);
    } else {
      tft.fillRect(x, y, cellW, cellH, pal.bg);
    }

    if (sel) {
      tft.drawRoundRect(x + cellMargin, y + cellMargin, innerW, innerH, 5, pal.fg);
      tft.drawRoundRect(x + cellMargin + 1, y + cellMargin + 1, innerW - 2, innerH - 2, 5, pal.fg);
    }

    drawIconBitmap(icons[i], cx, iconCy, c, 1.0);

    String label = labels[i];
    if ((int)label.length() > maxLabelChars && maxLabelChars > 1) {
      label = label.substring(0, maxLabelChars - 1) + ".";
    }

    tft.setTextColor(c, sel ? pal.selectBg : pal.bg);
    tft.setTextSize(labelTextSize);
    int labelW = label.length() * labelCharW;
    int labelX = cx - labelW / 2;
    if (labelX < x + cellMargin) labelX = x + cellMargin;
    int labelY = y + cellMargin + iconAreaH + (labelAreaH - lineH) / 2;
    tft.setCursor(labelX, labelY);
    tft.print(label);
  };

  bool canPartialUpdate = sameMenu
      && iconMenuLastTopRow == iconMenuTopRow
      && iconMenuLastSelected >= 0
      && iconMenuLastSelected != selectedIndex
      && iconMenuLastSelected / cols >= iconMenuTopRow && iconMenuLastSelected / cols < iconMenuTopRow + visibleRows
      && selRow >= iconMenuTopRow && selRow < iconMenuTopRow + visibleRows;

  if (canPartialUpdate) {
    drawCell(iconMenuLastSelected, false);
    drawCell(selectedIndex, true);
  } else {
    tft.fillScreen(pal.bg);
    drawScanlines(HEADER_H + 2);
    displayDrawHeaderBar(title);

    for (int i = 0; i < n; i++) {
      int row = i / cols;
      if (row < iconMenuTopRow || row >= iconMenuTopRow + visibleRows) continue;
      drawCell(i, i == selectedIndex);
    }

    if (scrolling) {
      int trackX = tft.width() - 3;
      int trackH = shownRows * cellH;
      tft.drawRect(trackX, gridTop, 3, trackH, pal.fgDim);
      int thumbH = max(8, trackH * visibleRows / totalRows);
      int thumbY = gridTop + (trackH - thumbH) * iconMenuTopRow / max(1, maxTopRow);
      tft.fillRect(trackX, thumbY, 3, thumbH, pal.fg);
    }
    displayDrawBorder();
  }

  iconMenuLastTitle = title ? title : "";
  iconMenuLastIcons = icons;
  iconMenuLastLabels = labels;
  iconMenuLastSelected = selectedIndex;
  iconMenuLastTopRow = iconMenuTopRow;
  iconMenuLastThemeVersion = themeVersion;
}
