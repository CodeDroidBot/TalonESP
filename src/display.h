#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <vector>

extern TFT_eSPI tft;

void displayInit();
void displayClear();
void displayShowMenu(const char* title, const std::vector<String>& items, int selectedIndex);
void displayShowMessage(const char* title, const char* msg);

// Boot splash: big skull icon centered, "System Init" text underneath.
void displayShowBootSplash(const char* deviceName);

void displayDrawHeaderBar(const char* title);
int displayHeaderHeight();

void displayDrawFooterBar(const char* hint);
int displayFooterHeight();

// Draw the thin 1px outline around the whole panel. Called automatically
// by the header/footer/menu draw functions; exposed publicly so custom
// screens can redraw it after doing their own fills.
void displayDrawBorder();

// -----------------------------------------------------------------------
// Status indicators shown in the header bar
// -----------------------------------------------------------------------
// SD card presence — drives the green/red SD icon in the header.
void displaySetSdMounted(bool mounted);
bool displayGetSdMounted();

// Battery level (0-100) and optional charging flag — drives the battery
// icon in the header. Call as often as you like; cheap to redraw.
void displaySetBatteryLevel(uint8_t percent, bool charging = false);

// -----------------------------------------------------------------------
// Layout grid
// -----------------------------------------------------------------------
int displayMargin();
int displayRowHeight();

String displayFitText(const String& text, int maxWidthPx, int textSize);

uint16_t displayColorBg();
uint16_t displayColorFg();
uint16_t displayColorFgDim();
uint16_t displayColorHeaderBg();
uint16_t displayColorSelectBg();
uint16_t displayColorSelectFg();

// -----------------------------------------------------------------------
// Backlight
// -----------------------------------------------------------------------
void displaySetBacklight(uint8_t brightness);
uint8_t displayGetBacklight();

void displaySetRotation(uint8_t rotation);
uint8_t displayGetRotation();

// -----------------------------------------------------------------------
// Theming
// -----------------------------------------------------------------------
enum class ThemeStyle {
  OrangeBlack,
  TerminalGreen,
  OrangeGrey,
  Count
};

enum class ThemeMode {
  Dark,
  Light,
  Count
};

void displayCycleTheme();
void displayToggleMode();
void displaySetTheme(ThemeStyle style, ThemeMode mode);
ThemeStyle displayGetThemeStyle();
ThemeMode  displayGetThemeMode();
const char* displayGetThemeName();
const char* displayGetModeName();

void displaySetUserBrightness(uint8_t brightness);
void displaySetBacklightTimeout(uint16_t seconds);
uint16_t displayGetBacklightTimeout();
void displayResetIdleTimer();
void displayCheckIdle();
bool displayIsDimmed();

void     displayTick();
uint16_t displayGetRgbHue();
void     displaySetRgbSpeed(uint8_t degPerTick);
uint8_t  displayGetRgbSpeed();

// -----------------------------------------------------------------------
// Icon home screen
// -----------------------------------------------------------------------
enum class MenuIcon {
  SubGhz, Infrared, Files, Wifi, Ble, UsbHid, Games, Settings, About,
  Gps, Lora,
  Deauth, Beacon, Script, Layout
};

void displayShowIconMenu(const char* title, const std::vector<MenuIcon>& icons,
                          const std::vector<String>& labels, int selectedIndex);

// -----------------------------------------------------------------------
// Misc UI helpers
// -----------------------------------------------------------------------
void drawStatusBar(int batteryLevel, const char* timeStr = nullptr);
void drawProgressBar(int x, int y, int w, int h, float progress, uint16_t barColor, uint16_t bgColor);
void drawSpinner(int cx, int cy, int radius, uint16_t color, int frame);
void drawListItem(int x, int y, int w, int h, const String& label,
                   const String& value, bool selected, bool hasValue);
String alignLeft(const String& text, int width, char padChar = ' ');
String alignRight(const String& text, int width, char padChar = ' ');
String alignCenter(const String& text, int width, char padChar = ' ');