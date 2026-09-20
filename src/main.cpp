#include <Arduino.h>
#include <cmath>
#include <cstring>
#include "config.h"
#include "display.h"
#include "buttons.h"
#include "sdcard.h"
#include "ir_module.h"
#include "rf_module.h"
#include "wifi_module.h"
#include "ble/ble_scan.h"
#include "ble/ble_spam.h"
#include "games.h"
#include "settings.h"
#include "menu.h"
#include "gps_module.h"
#include "lora_module.h"
#include "badusb/hid_module.h"
#include "wireguard_module.h"
#include "gpio_module.h"
#include "helpers.h"
#include "shared.h"
#include "web_ui.h"

// ---- GPIO sub-modules ----
#include "spi/spi_module.h"
#include "uart/uart_module.h"
#include "threewire/threewire_module.h"
#include "nrf/nrf_module.h"
#include "canbus/can_module.h"
#include "pn532/pn532_nfc.h"
#include "pn532/pn532_rfid.h"

// ---- Brownout detector control (needed for reliable BLE spam) ----
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_system.h"

// Override the Wi-Fi library's sanity check to allow deauth frame injection
extern "C" int ieee80211_raw_frame_sanity_check(int32_t arg1, int32_t arg2, int32_t arg3) {
    return 0;  // Always allow transmission
}

// ====================================================================
//  AppState enum
// ====================================================================
enum AppState {
  ST_MAIN_MENU               = STATE_MAIN_MENU,
  ST_SUBGHZ_MENU             = STATE_SUBGHZ_MENU,
  ST_SUBGHZ_LISTEN           = STATE_SUBGHZ_LISTEN,
  ST_SUBGHZ_SCAN             = STATE_SUBGHZ_SCAN,
  ST_SUBGHZ_INFO             = STATE_SUBGHZ_INFO,
  ST_SUBGHZ_JAM              = STATE_SUBGHZ_JAM,
  ST_SUBGHZ_SAVED            = STATE_SUBGHZ_SAVED,
  ST_SUBGHZ_SAVED_ACTION     = STATE_SUBGHZ_SAVED_ACTION,
  ST_INFRARED_MENU           = STATE_INFRARED_MENU,
  ST_INFRARED_CAPTURE        = STATE_INFRARED_CAPTURE,
  ST_INFRARED_UNIVERSAL_CATEGORY = STATE_INFRARED_UNIVERSAL_CATEGORY,
  ST_INFRARED_UNIVERSAL_BRAND    = STATE_INFRARED_UNIVERSAL_BRAND,
  ST_INFRARED_UNIVERSAL      = STATE_INFRARED_UNIVERSAL,
  ST_INFRARED_PLAY_FILE      = STATE_INFRARED_PLAY_FILE,
  ST_INFRARED_TVBGONE        = STATE_INFRARED_TVBGONE,
  ST_INFRARED_JAMMER         = STATE_INFRARED_JAMMER,
  ST_FILE_MANAGER            = STATE_FILE_MANAGER,
  ST_FILE_ACTION             = STATE_FILE_ACTION,
  ST_WIFI_MENU               = STATE_WIFI_MENU,
  ST_WIFI_SCAN               = STATE_WIFI_SCAN,
  ST_WIFI_METER              = STATE_WIFI_METER,
  ST_WIFI_DEAUTH             = STATE_WIFI_DEAUTH,
  ST_WIFI_DEAUTH_PICK        = STATE_WIFI_DEAUTH_PICK,
  ST_WIFI_DEAUTH_RUN         = STATE_WIFI_DEAUTH_RUN,
  ST_WIFI_BEACON_PICK        = STATE_WIFI_BEACON_PICK,
  ST_WIFI_BEACON_RUN         = STATE_WIFI_BEACON_RUN,
  ST_WIFI_PACKET_MON         = STATE_WIFI_PACKET_MON,
  ST_WIFI_HANDSHAKE_MENU     = STATE_WIFI_HANDSHAKE_MENU,
  ST_WIFI_HANDSHAKE_CAPTURE  = STATE_WIFI_HANDSHAKE_CAPTURE,
  ST_WIFI_HANDSHAKE_RESULT   = STATE_WIFI_HANDSHAKE_RESULT,
  ST_BLE_MENU                = STATE_BLE_MENU,
  ST_BLE_SCAN                = STATE_BLE_SCAN,
  ST_BLE_AIRTAG              = STATE_BLE_AIRTAG,
  ST_BLE_SKIMMER             = STATE_BLE_SKIMMER,
  ST_BLE_SPAM                = STATE_BLE_SPAM,
  ST_HID_MENU                = STATE_HID_MENU,
  ST_HID_SELECT_MODE         = STATE_HID_SELECT_MODE,
  ST_HID_KEYBOARD            = STATE_HID_KEYBOARD,
  ST_HID_MOUSE               = STATE_HID_MOUSE,
  ST_HID_SCRIPT_SELECT       = STATE_HID_SCRIPT_SELECT,
  ST_HID_SCRIPT_RUN          = STATE_HID_SCRIPT_RUN,
  ST_GAMES_MENU              = STATE_GAMES_MENU,
  ST_GAME_SNAKE              = STATE_GAME_SNAKE,
  ST_GAME_TETRIS             = STATE_GAME_TETRIS,
  ST_GAME_PONG               = STATE_GAME_PONG,
  ST_SETTINGS                = STATE_SETTINGS,
  ST_ABOUT                   = STATE_ABOUT,
  ST_GPS_MENU                = STATE_GPS_MENU,
  ST_GPS_LIVE                = STATE_GPS_LIVE,
  ST_GPS_LOG                 = STATE_GPS_LOG,
  ST_GPS_WAYPOINTS           = STATE_GPS_WAYPOINTS,
  ST_GPS_WAYPOINT_VIEW       = STATE_GPS_WAYPOINT_VIEW,
  ST_LORA_MENU               = STATE_LORA_MENU,
  ST_LORA_CHAT               = STATE_LORA_CHAT,
  ST_LORA_MONITOR            = STATE_LORA_MONITOR,
  ST_LORA_SETTINGS           = STATE_LORA_SETTINGS,
  ST_HID_SCRIPT_PICKER       = STATE_HID_SCRIPT_PICKER,
  ST_IR_PLAY_PICKER          = STATE_IR_PLAY_PICKER,
  ST_WIREGUARD               = STATE_WIREGUARD,
  ST_WIFI_CHANNEL_ANALYZER   = STATE_WIFI_CHANNEL_ANALYZER,
  ST_WIFI_SNIFFER            = STATE_WIFI_SNIFFER,
  ST_WIFI_CONNECT            = STATE_WIFI_CONNECT,
  ST_WIFI_SCANHOSTS          = STATE_WIFI_SCANHOSTS,
  ST_GPS_WARDRIVE            = STATE_GPS_WARDRIVE,
  ST_GPIO_MENU               = STATE_GPIO_MENU,
  ST_GPIO_DIO                = STATE_GPIO_DIO,
  ST_GPIO_EXPANDER           = STATE_GPIO_EXPANDER,
  ST_GPIO_I2C_MENU           = STATE_GPIO_I2C_MENU,
  ST_GPIO_I2C_SCAN           = STATE_GPIO_I2C_SCAN,
  ST_GPIO_SPI_MENU           = STATE_GPIO_SPI_MENU,
  ST_GPIO_SPI_PROBE          = STATE_GPIO_SPI_PROBE,
  ST_GPIO_UART_MENU          = STATE_GPIO_UART_MENU,
  ST_GPIO_1WIRE              = STATE_GPIO_1WIRE,
  ST_GPIO_2WIRE              = STATE_GPIO_2WIRE,
  ST_GPIO_3WIRE              = STATE_GPIO_3WIRE,
  ST_GPIO_NFC                = STATE_GPIO_NFC,
  ST_GPIO_UART_ACTIVE        = STATE_GPIO_UART_ACTIVE,
  ST_GPIO_NRF                = STATE_GPIO_NRF,
  ST_GPIO_CAN                = STATE_GPIO_CAN,
  ST_GPIO_RFID               = STATE_GPIO_RFID,
  ST_WIFI_EVIL_PORTAL        = STATE_WIFI_EVIL_PORTAL,
  ST_WEB_UI                  = STATE_WEB_UI,

  ST_I2C_READ                = STATE_I2C_READ,
  ST_I2C_WRITE               = STATE_I2C_WRITE,
  ST_I2C_WRITEREAD           = STATE_I2C_WRITEREAD,
  ST_I2C_SPEED               = STATE_I2C_SPEED,
  ST_I2C_SLAVE               = STATE_I2C_SLAVE,

  ST_SPI_TRANSFER            = STATE_SPI_TRANSFER,
  ST_SPI_MODE                = STATE_SPI_MODE,
  ST_SPI_FREQ                = STATE_SPI_FREQ,
  ST_SPI_SLAVE               = STATE_SPI_SLAVE,

  ST_UART_TERMINAL           = STATE_UART_TERMINAL,
  ST_UART_SETTINGS           = STATE_UART_SETTINGS,

  ST_THREEWIRE_READ          = STATE_THREEWIRE_READ,
  ST_THREEWIRE_WRITE         = STATE_THREEWIRE_WRITE,
  ST_THREEWIRE_ERASE         = STATE_THREEWIRE_ERASE,
};

static AppState state = ST_MAIN_MENU;

// ====================================================================
//  Global menus
// ====================================================================
static SimpleMenu mainMenu({"Sub-GHz", "IR", "Files", "WiFi", "BLE", "badUSB", "GPIO", "Games", "Settings", "About", "GPS", "LoRa", "Web UI"});
static const std::vector<MenuIcon> mainMenuIcons = {
  MenuIcon::SubGhz, MenuIcon::Infrared, MenuIcon::Files, MenuIcon::Wifi,
  MenuIcon::Ble,    MenuIcon::UsbHid,   MenuIcon::UsbHid, MenuIcon::Games,
  MenuIcon::Settings, MenuIcon::About,  MenuIcon::Gps,   MenuIcon::Lora,
  MenuIcon::Layout
};

static SimpleMenu infraredMenu({"TV-B-Gone", "Custom IR", "IR Read", "IR Jammer", "Universal Remote"});
static SimpleMenu bleMenu({"Scan", "AirTag Sniffer", "Skimmer Detect", "BLE Spam"});
static SimpleMenu gamesMenu({"Snake", "Tetris", "Pong"});
static SimpleMenu fileMenu({});
static SimpleMenu fileActionMenu({"View", "Replay IR", "Replay RF", "Delete", "Cancel"});
static String selectedFileName;
static String currentDir;
int g_rfNextState = 0;

// ====================================================================
//  Drawing functions
// ====================================================================
static void drawMainMenu() {
  displayShowIconMenu(DEVICE_NAME, mainMenuIcons, mainMenu.items(), mainMenu.index());
}

static String currentDirFullPath() {
  return currentDir.length() == 0 ? "/" : ("/" + currentDir);
}

static void drawFileManagerScreen() {
  String path = currentDir.length() == 0 ? "" : ("/" + currentDir);
  String title;
  if (state == ST_IR_PLAY_PICKER)         title = "Custom IR" + path;
  else if (state == ST_HID_SCRIPT_PICKER) title = "Run DuckyScript" + path;
  else                                    title = (path.length() ? path : "SD Files");
  displayShowMenu(title.c_str(), fileMenu.items(), fileMenu.index());
}

// ====================================================================
//  BadUSB progress screen
// ====================================================================
static void drawBadUsbRunScreen() {
  tft.fillScreen(displayColorBg());
  displayDrawHeaderBar("DuckyScript");

  int y = displayHeaderHeight() + 12;
  tft.setTextColor(displayColorFg(), displayColorBg());
  tft.setTextSize(1);
  tft.setCursor(8, y);
  tft.print("Status: " + hidScriptGetStatus());
  y += 16;
  tft.setCursor(8, y);
  tft.print("Line " + String(hidScriptGetLineNumber()) + " / " + String(hidScriptGetTotalLines()));
  y += 20;

  int pct = hidScriptGetProgress();
  drawProgressBar(8, y, tft.width() - 16, 12, pct / 100.0f,
                  displayColorFg(), displayColorBg());
  y += 20;
  tft.setCursor(8, y);
  tft.print(String(pct) + "%");

  displayDrawFooterBar("BACK: stop script");
}

// ====================================================================
//  enterState()
// ====================================================================
void enterState(int stateId) {
  AppState s = static_cast<AppState>(stateId);
  state = s;
  switch (s) {
    case ST_MAIN_MENU:      drawMainMenu(); break;
    case ST_SUBGHZ_MENU:
    case ST_SUBGHZ_LISTEN:
    case ST_SUBGHZ_SCAN:
    case ST_SUBGHZ_INFO:
    case ST_SUBGHZ_JAM:
    case ST_SUBGHZ_SAVED:
    case ST_SUBGHZ_SAVED_ACTION:
      rfEnterMenu();
      break;
    case ST_INFRARED_MENU:
      displayShowMenu("Infrared", infraredMenu.items(), infraredMenu.index());
      break;
    case ST_INFRARED_CAPTURE: irEnterCaptureScreen(); break;
    case ST_INFRARED_UNIVERSAL_CATEGORY: irEnterUniversalRemote(); break;
    case ST_INFRARED_UNIVERSAL_BRAND:
    case ST_INFRARED_UNIVERSAL: break;
    case ST_INFRARED_PLAY_FILE: irEnterPlayFile(); break;
    case ST_INFRARED_TVBGONE: irEnterTvBGone(); break;
    case ST_INFRARED_JAMMER: irEnterJammer(); break;

    case ST_FILE_MANAGER:
    case ST_HID_SCRIPT_PICKER:
    case ST_IR_PLAY_PICKER:
    {
      irSignalPickerBack();
      auto files = sdListDir(currentDirFullPath().c_str());
      std::vector<String> labels;
      if (currentDir.length() > 0) labels.push_back(".. (up one level)");
      for (auto &f : files) {
        bool isDir = f.endsWith("/");
        if (s == ST_IR_PLAY_PICKER && !isDir) {
          String lower = f; lower.toLowerCase();
          if (!lower.endsWith(".ir")) continue;
        }
        if (s == ST_HID_SCRIPT_PICKER && !isDir) {
          String lower = f; lower.toLowerCase();
          if (!lower.endsWith(".txt") && !lower.endsWith(".duck")) continue;
        }
        labels.push_back(f);
      }
      if (labels.empty()) {
        if (s == ST_IR_PLAY_PICKER)         labels.push_back("(no .ir files here)");
        else if (s == ST_HID_SCRIPT_PICKER) labels.push_back("(no .txt/.duck scripts here)");
        else                                labels.push_back("(empty, or no SD card)");
      }
      fileMenu.setItems(labels);
      drawFileManagerScreen();
      break;
    }

    case ST_FILE_ACTION:
      displayShowMenu(selectedFileName.c_str(), fileActionMenu.items(), fileActionMenu.index());
      break;

    case ST_WIFI_MENU:          wifiEnterMenu(); break;
    case ST_WIFI_SCAN:          wifiEnterScan(); break;
    case ST_WIFI_METER:         wifiEnterMeter(); break;
    case ST_WIFI_PACKET_MON:    wifiEnterPacketMonitor(); break;
    case ST_WIFI_DEAUTH:        wifiEnterDeauthDetect(); break;
    case ST_WIFI_DEAUTH_PICK:   wifiEnterDeauthPick(); break;
    case ST_WIFI_DEAUTH_RUN:    wifiEnterDeauthRun(); break;
    case ST_WIFI_BEACON_PICK:   wifiEnterBeaconPick(); break;
    case ST_WIFI_BEACON_RUN:    wifiEnterBeaconRun(); break;
    case ST_WIFI_CHANNEL_ANALYZER: wifiEnterChannelAnalyzer(); break;
    case ST_WIFI_SNIFFER:       wifiEnterSniffer(); break;
    case ST_WIFI_CONNECT:       wifiEnterConnect(); break;
    case ST_WIFI_SCANHOSTS:     wifiEnterScanHosts(); break;
    case ST_WIREGUARD:          wgEnterMenu(); break;
    case ST_WIFI_EVIL_PORTAL:   wifiEnterEvilPortal(); break;
    case ST_WIFI_HANDSHAKE_MENU: wifiEnterHandshakeMenu(); break;
    case ST_WIFI_HANDSHAKE_CAPTURE: wifiEnterHandshakeCapture(); break;
    case ST_WIFI_HANDSHAKE_RESULT: wifiEnterHandshakeResult(); break;

    case ST_BLE_MENU:
      displayShowMenu("BLE", bleMenu.items(), bleMenu.index());
      break;
    case ST_BLE_SCAN: {
      auto devices = bleScanDevices();
      String msg = "";
      for (auto &d : devices) msg += d + "\n";
      displayShowMessage("BLE Scan (OK=rescan)", msg.c_str());
      break;
    }
    case ST_BLE_AIRTAG: {
      auto devices = bleScanAirTags();
      String msg = "";
      for (auto &d : devices) msg += d + "\n";
      displayShowMessage("AirTag Sniffer (OK=rescan)", msg.c_str());
      break;
    }
    case ST_BLE_SKIMMER: {
      auto devices = bleScanSkimmers();
      String msg = "";
      for (auto &d : devices) msg += d + "\n";
      displayShowMessage("Skimmer Detect (OK=rescan)", msg.c_str());
      break;
    }
    case ST_BLE_SPAM: bleSpamEnterMenu(); break;

    case ST_HID_MENU:           hidEnterMenu(); break;
    case ST_HID_SELECT_MODE:    hidEnterModeSelect(); break;
    case ST_HID_KEYBOARD:       hidEnterKeyboard(); break;
    case ST_HID_MOUSE:          hidEnterMouse(); break;
    case ST_HID_SCRIPT_SELECT:  hidEnterScriptSelect(); break;
    case ST_HID_SCRIPT_RUN:     hidEnterScriptRun(); drawBadUsbRunScreen(); break;

    case ST_GAMES_MENU: displayShowMenu("Games", gamesMenu.items(), gamesMenu.index()); break;
    case ST_GAME_SNAKE:  snakeInit();  break;
    case ST_GAME_TETRIS: tetrisInit(); break;
    case ST_GAME_PONG:   pongInit();   break;

    case ST_SETTINGS: settingsInit(); break;

    case ST_ABOUT: {
      String msg = String(DEVICE_NAME) +
        "\nESP32-S3 Multi-Tool\nTFT+SD+CC1101+IR+MCP23017\nWiFi+BLE+GPS+LoRa+Games\nBuilt with PlatformIO";
      displayShowMessage("About", msg.c_str());
      break;
    }

    case ST_GPS_MENU:           gpsEnterMenu(); break;
    case ST_GPS_LIVE:           gpsEnterLive(); break;
    case ST_GPS_LOG:            gpsEnterLog(); break;
    case ST_GPS_WAYPOINTS:      gpsEnterWaypoints(); break;
    case ST_GPS_WAYPOINT_VIEW:  gpsEnterWaypointView(); break;
    case ST_GPS_WARDRIVE:       gpsEnterWardrive(); break;

    case ST_LORA_MENU:          loraEnterMenu(); break;
    case ST_LORA_CHAT:          loraEnterChat(); break;
    case ST_LORA_MONITOR:       loraEnterMonitor(); break;
    case ST_LORA_SETTINGS:      loraEnterSettings(); break;

    case ST_GPIO_MENU:          gpioEnterMenu(); break;
    case ST_GPIO_DIO:           gpioEnterDio(); break;
    case ST_GPIO_EXPANDER:      gpioEnterExpander(); break;
    case ST_GPIO_I2C_MENU:      gpioEnterI2CMenu(); break;
    case ST_GPIO_I2C_SCAN:      gpioEnterI2CScan(); break;
    case ST_GPIO_SPI_MENU:      gpioEnterSPIMenu(); break;
    case ST_GPIO_SPI_PROBE:     gpioEnterSPIProbe(); break;
    case ST_GPIO_UART_MENU:     gpioEnterUARTMenu(); break;
    case ST_GPIO_1WIRE:         gpioEnter1Wire(); break;
    case ST_GPIO_2WIRE:         gpioEnter2Wire(); break;
    case ST_GPIO_3WIRE:         threewireEnterMenu(); break;
    case ST_GPIO_NFC:           gpioEnterNFC(); break;
    case ST_GPIO_RFID:          gpioEnterRFID(); break;
    case ST_GPIO_UART_ACTIVE:   gpioEnterUARTActive(); break;
    case ST_GPIO_NRF:           gpioEnterNRF(); break;
    case ST_GPIO_CAN:           gpioEnterCAN(); break;

    case ST_WEB_UI:             webStartServer(); break;

    case ST_I2C_READ:           i2cEnterRead(); break;
    case ST_I2C_WRITE:          i2cEnterWrite(); break;
    case ST_I2C_WRITEREAD:      i2cEnterWriteRead(); break;
    case ST_I2C_SPEED:          i2cEnterSpeed(); break;
    case ST_I2C_SLAVE:          i2cEnterSlave(); break;

    case ST_SPI_TRANSFER:       spiEnterTransfer(); break;
    case ST_SPI_MODE:           spiEnterMode(); break;
    case ST_SPI_FREQ:           spiEnterFreq(); break;
    case ST_SPI_SLAVE:          spiEnterSlave(); break;

    case ST_UART_TERMINAL:      uartEnterTerminal(); break;
    case ST_UART_SETTINGS:      uartEnterSettings(); break;

    case ST_THREEWIRE_READ:     threewireEnterRead(); break;
    case ST_THREEWIRE_WRITE:    threewireEnterWrite(); break;
    case ST_THREEWIRE_ERASE:    threewireEnterErase(); break;
  }
}

// ====================================================================
//  setup()
// ====================================================================
void setup() {
    Serial.begin(115200);
    delay(400);

    Serial.printf("[BOOT] Reset reason: %d\n", (int)esp_reset_reason());

    displayInit();
    displayShowBootSplash(DEVICE_NAME);
    delay(900);

    bleSpamDisableBrownout();
    buttonsInit();

    bool sdOk = sdInit();
    displaySetSdMounted(sdOk);
    displaySetBatteryLevel(100);

    if (sdOk) settingsLoad();

    irInit();
    hidInit();
    gpioModuleInit();
    wifiInit();

    rfProfilesInit();
    powerInit();

    Serial.println("─────────────────────────────────────");
    Serial.printf("[MEM] Flash used:      %u / %u bytes\n",
                  ESP.getSketchSize(),
                  ESP.getSketchSize() + ESP.getFreeSketchSpace());
    Serial.printf("[MEM] Heap free:      %u bytes\n", ESP.getFreeHeap());
    Serial.printf("[MEM] Heap min free:  %u bytes\n", ESP.getMinFreeHeap());
    Serial.printf("[MEM] PSRAM total:    %u bytes\n", ESP.getPsramSize());
    Serial.printf("[MEM] PSRAM free:     %u bytes\n", ESP.getFreePsram());
    Serial.println("─────────────────────────────────────");

    enterState(STATE_MAIN_MENU);
}

// ====================================================================
//  loop()
// ====================================================================
void loop() {
  int evt = buttonsPoll();

  if (evt != EVT_NONE) {
    displayResetIdleTimer();
  }

  settingsTick();
  gpsUpdate();
  gpsLogTick();
  gpsWardriveTick();

  String loraIncoming;
  int loraIncRssi = 0;
  float loraIncSnr = 0.0f;
  bool loraGotPacket = loraPoll(loraIncoming, loraIncRssi, loraIncSnr);

  if (state == ST_GPIO_UART_ACTIVE || state == ST_UART_TERMINAL) {
    gpioUARTBridgeTick();
  }

  // ---- Main menu ----
  if (state == ST_MAIN_MENU) {
    int idx = mainMenu.index();
    if (evt == EVT_UP)    { mainMenu.setIndex(idx - 2); drawMainMenu(); }
    if (evt == EVT_DOWN)  { mainMenu.setIndex(idx + 2); drawMainMenu(); }
    if (evt == EVT_LEFT)  { mainMenu.setIndex(idx - 1); drawMainMenu(); }
    if (evt == EVT_RIGHT) { mainMenu.setIndex(idx + 1); drawMainMenu(); }
    if (evt == EVT_OK) {
      switch (mainMenu.index()) {
        case 0: enterState(STATE_SUBGHZ_MENU);   break;
        case 1: enterState(STATE_INFRARED_MENU); break;
        case 2: currentDir = ""; enterState(STATE_FILE_MANAGER); break;
        case 3: enterState(STATE_WIFI_MENU);     break;
        case 4: enterState(STATE_BLE_MENU);      break;
        case 5: enterState(STATE_HID_MENU);      break;
        case 6: enterState(STATE_GPIO_MENU);     break;
        case 7: enterState(STATE_GAMES_MENU);    break;
        case 8: enterState(STATE_SETTINGS);      break;
        case 9: enterState(STATE_ABOUT);         break;
        case 10: enterState(STATE_GPS_MENU);     break;
        case 11: enterState(STATE_LORA_MENU);    break;
        case 12: enterState(STATE_WEB_UI);       break;
      }
    }
    displayCheckIdle();
    return;
  }

  // ---- Global BACK ----
  if (evt == EVT_BACK) {
    powerExitState(state);

    if (state == ST_IR_PLAY_PICKER) {
      if (irIsSignalPickerActive()) {
        irSignalPickerBack();
        drawFileManagerScreen();
        displayCheckIdle();
        return;
      }
      if (irIsInFileActions()) {
        irExitFileActions();
        displayCheckIdle();
        return;
      }
      enterState(STATE_INFRARED_MENU);
      displayCheckIdle();
      return;
    }

    if (state == ST_HID_SCRIPT_PICKER) {
      g_scriptPickerMode = false;
      enterState(STATE_HID_MENU);
      displayCheckIdle();
      return;
    }

    if (state == ST_INFRARED_UNIVERSAL_CATEGORY ||
        state == ST_INFRARED_UNIVERSAL_BRAND ||
        state == ST_INFRARED_UNIVERSAL) {
      irHandleUniversalEvent(EVT_BACK);
      if (!irIsUniversalRemoteActive()) enterState(STATE_INFRARED_MENU);
      displayCheckIdle();
      return;
    }

    switch (state) {
      case ST_INFRARED_CAPTURE:
      case ST_INFRARED_PLAY_FILE:
      case ST_INFRARED_TVBGONE:
      case ST_INFRARED_JAMMER:
        enterState(STATE_INFRARED_MENU); break;

      case ST_WIFI_SCAN:
      case ST_WIFI_METER:
      case ST_WIFI_PACKET_MON:
      case ST_WIFI_DEAUTH:
      case ST_WIFI_DEAUTH_PICK:
      case ST_WIFI_DEAUTH_RUN:
      case ST_WIFI_BEACON_PICK:
      case ST_WIFI_BEACON_RUN:
      case ST_WIFI_CHANNEL_ANALYZER:
      case ST_WIFI_SNIFFER:
      case ST_WIFI_CONNECT:
      case ST_WIFI_SCANHOSTS:
      case ST_WIREGUARD:
      case ST_WIFI_EVIL_PORTAL:
      case ST_WIFI_HANDSHAKE_MENU:
      case ST_WIFI_HANDSHAKE_CAPTURE:
      case ST_WIFI_HANDSHAKE_RESULT:
        enterState(STATE_WIFI_MENU); break;

      case ST_BLE_SCAN:
      case ST_BLE_AIRTAG:
      case ST_BLE_SKIMMER:
      case ST_BLE_SPAM:
        bleSpamStop();
        enterState(STATE_BLE_MENU); break;

      case ST_GPS_LIVE:
      case ST_GPS_LOG:
      case ST_GPS_WAYPOINTS:
      case ST_GPS_WAYPOINT_VIEW:
      case ST_GPS_WARDRIVE:
        enterState(STATE_GPS_MENU); break;

      case ST_LORA_CHAT:
      case ST_LORA_MONITOR:
      case ST_LORA_SETTINGS:
        enterState(STATE_LORA_MENU); break;

      case ST_GPIO_DIO:
      case ST_GPIO_EXPANDER:
      case ST_GPIO_1WIRE:
      case ST_GPIO_2WIRE:
      case ST_GPIO_NFC:
      case ST_GPIO_RFID:
      case ST_GPIO_NRF:
      case ST_GPIO_CAN:
        enterState(STATE_GPIO_MENU); break;

      case ST_GPIO_I2C_MENU:
      case ST_GPIO_SPI_MENU:
      case ST_GPIO_UART_MENU:
      case ST_GPIO_3WIRE:
        enterState(STATE_GPIO_MENU); break;

      case ST_GPIO_I2C_SCAN:
        enterState(STATE_GPIO_I2C_MENU); break;
      case ST_GPIO_SPI_PROBE:
        enterState(STATE_GPIO_SPI_MENU); break;
      case ST_GPIO_UART_ACTIVE:
        enterState(STATE_GPIO_UART_MENU); break;

      case ST_I2C_READ:
      case ST_I2C_WRITE:
      case ST_I2C_WRITEREAD:
      case ST_I2C_SPEED:
      case ST_I2C_SLAVE:
        enterState(STATE_GPIO_I2C_MENU); break;

      case ST_SPI_TRANSFER:
      case ST_SPI_MODE:
      case ST_SPI_FREQ:
      case ST_SPI_SLAVE:
        enterState(STATE_GPIO_SPI_MENU); break;

      case ST_UART_TERMINAL:
      case ST_UART_SETTINGS:
        enterState(STATE_GPIO_UART_MENU); break;

      case ST_THREEWIRE_READ:
      case ST_THREEWIRE_WRITE:
      case ST_THREEWIRE_ERASE:
        enterState(STATE_GPIO_3WIRE); break;

      case ST_HID_SELECT_MODE:
      case ST_HID_KEYBOARD:
      case ST_HID_MOUSE:
      case ST_HID_SCRIPT_SELECT:
      case ST_HID_SCRIPT_RUN:
        hidStopScript();
        hidSetMode(HID_MODE_OFF);
        enterState(STATE_HID_MENU); break;

      case ST_GAME_SNAKE:
      case ST_GAME_TETRIS:
      case ST_GAME_PONG:
        enterState(STATE_GAMES_MENU); break;

      case ST_SUBGHZ_LISTEN:
      case ST_SUBGHZ_SCAN:
      case ST_SUBGHZ_INFO:
      case ST_SUBGHZ_JAM:
      case ST_SUBGHZ_SAVED:
      case ST_SUBGHZ_SAVED_ACTION:
        enterState(STATE_SUBGHZ_MENU); break;

      case ST_SETTINGS:
      case ST_ABOUT:
        enterState(STATE_MAIN_MENU); break;

      case ST_FILE_ACTION:
        enterState(STATE_FILE_MANAGER); break;

      case ST_WEB_UI:
        webStopServer();
        enterState(STATE_MAIN_MENU); break;

      default:
        enterState(STATE_MAIN_MENU); break;
    }
    displayCheckIdle();
    return;
  }

  // ---- Sub-GHz ----
  if (state == ST_SUBGHZ_MENU || state == ST_SUBGHZ_LISTEN || state == ST_SUBGHZ_SCAN ||
      state == ST_SUBGHZ_INFO || state == ST_SUBGHZ_JAM || state == ST_SUBGHZ_SAVED ||
      state == ST_SUBGHZ_SAVED_ACTION) {
    rfHandleMenuEvent(evt);
  }

  // ---- Infrared ----
  if (state == ST_INFRARED_MENU) {
    if (evt == EVT_UP)   { infraredMenu.up();   enterState(STATE_INFRARED_MENU); }
    if (evt == EVT_DOWN) { infraredMenu.down(); enterState(STATE_INFRARED_MENU); }
    if (evt == EVT_OK) {
      switch (infraredMenu.index()) {
        case 0: enterState(STATE_INFRARED_TVBGONE); break;
        case 1: currentDir = "ir"; enterState(STATE_INFRARED_PLAY_FILE); break;
        case 2: enterState(STATE_INFRARED_CAPTURE); break;
        case 3: enterState(STATE_INFRARED_JAMMER); break;
        case 4: enterState(STATE_INFRARED_UNIVERSAL_CATEGORY); break;
      }
    }
  }
  if (state == ST_INFRARED_CAPTURE) {
    irReceiveLoop();
    irAutoTxLoop();
    irHandleCaptureEvent(evt);
  }
  if (state == ST_INFRARED_UNIVERSAL_CATEGORY ||
      state == ST_INFRARED_UNIVERSAL_BRAND ||
      state == ST_INFRARED_UNIVERSAL) {
    irUniversalSendingLoop();
    irHandleUniversalEvent(evt);
  }
  if (state == ST_INFRARED_TVBGONE) {
    irTvBGoneLoop();
    irHandleTvBGoneEvent(evt);
  }
  if (state == ST_INFRARED_JAMMER) {
    irJammerLoop();
    irHandleJammerEvent(evt);
  }

  // ---- File Manager / HID Script Picker / IR Play Picker ----
  if (state == ST_FILE_MANAGER || state == ST_HID_SCRIPT_PICKER || state == ST_IR_PLAY_PICKER) {
    if (irIsSignalPickerActive()) {
      irHandleSignalPickerEvent(evt);
      displayCheckIdle();
      return;
    }
    if (irSpamIsRunning()) {
      if (evt == EVT_OK || evt == EVT_BACK) irSpamStop();
      irSpamTick();
      displayCheckIdle();
      return;
    }
    if (irIsInFileActions()) {
      irHandleFileActionsEvent(evt);
      displayCheckIdle();
      return;
    }
    if (evt == EVT_UP)   { fileMenu.up();   drawFileManagerScreen(); }
    if (evt == EVT_DOWN) { fileMenu.down(); drawFileManagerScreen(); }
    if (evt == EVT_OK) {
      const auto &items = fileMenu.items();
      if (!items.empty()) {
        String name = items[fileMenu.index()];
        if (name == ".. (up one level)") {
          int slash = currentDir.lastIndexOf('/');
          currentDir = (slash >= 0) ? currentDir.substring(0, slash) : "";
          if (state == ST_HID_SCRIPT_PICKER) enterState(STATE_HID_SCRIPT_PICKER);
          else if (state == ST_IR_PLAY_PICKER) enterState(STATE_IR_PLAY_PICKER);
          else enterState(STATE_FILE_MANAGER);
        } else if (name.startsWith("(")) {
          // ignore placeholder
        } else if (name.endsWith("/")) {
          String dirName = name.substring(0, name.length() - 1);
          currentDir = (currentDir.length() > 0) ? (currentDir + "/" + dirName) : dirName;
          if (state == ST_HID_SCRIPT_PICKER) enterState(STATE_HID_SCRIPT_PICKER);
          else if (state == ST_IR_PLAY_PICKER) enterState(STATE_IR_PLAY_PICKER);
          else enterState(STATE_FILE_MANAGER);
        } else {
          selectedFileName = (currentDir.length() > 0) ? (currentDir + "/" + name) : name;
          String path = "/" + selectedFileName;
          if (state == ST_HID_SCRIPT_PICKER) {
            if (!usbHidIsReady()) usbHidStart();
            if (hidRunScript(path)) {
              g_scriptPickerMode = false;
              hidSetMode(HID_MODE_USB);
              enterState(STATE_HID_SCRIPT_RUN);
            } else {
              displayShowMessage("BadUSB", "Failed to load script");
              delay(900);
              enterState(STATE_HID_SCRIPT_PICKER);
            }
          } else if (state == ST_IR_PLAY_PICKER) {
            int count = irOpenFileForPicking(path.c_str());
            if (count <= 0) {
              displayShowMessage("IR", "Not a valid IR file");
              delay(500);
              enterState(STATE_IR_PLAY_PICKER);
              fileMenu.setIndex(fileMenu.index());
              drawFileManagerScreen();
            } else if (count == 1) {
              bool ok = irSendOpenedFileSignal(0);
              displayShowMessage("IR", ok ? ("Sent: " + name).c_str() : irLastSendError().c_str());
              delay(500);
              enterState(STATE_IR_PLAY_PICKER);
              fileMenu.setIndex(fileMenu.index());
              drawFileManagerScreen();
            } else {
              irEnterFileActions();
            }
          } else {
            fileActionMenu.setItems({"View", "Replay IR", "Replay RF", "Delete", "Cancel"});
            enterState(STATE_FILE_ACTION);
          }
        }
      }
    }
    displayCheckIdle();
    return;
  }

  if (state == ST_FILE_ACTION) {
    if (evt == EVT_UP)   { fileActionMenu.up();   enterState(STATE_FILE_ACTION); }
    if (evt == EVT_DOWN) { fileActionMenu.down(); enterState(STATE_FILE_ACTION); }
    if (evt == EVT_OK) {
      String path = "/" + selectedFileName;
      switch (fileActionMenu.index()) {
        case 0: {
          String content;
          bool ok = sdReadFile(path.c_str(), content);
          if (ok && content.length() > 900) content = content.substring(0, 900) + "\n...(truncated)";
          displayShowMessage(selectedFileName.c_str(), ok ? content.c_str() : "Could not read file");
          delay(2000);
          enterState(STATE_FILE_ACTION);
          break;
        }
        case 1: {
          bool ok = irReplayFromSD(path.c_str());
          displayShowMessage("Infrared", ok ? "Replayed" : "Not a valid IR file");
          delay(600);
          enterState(STATE_FILE_ACTION);
          break;
        }
        case 2: {
          bool ok = rfReplayFromSD(path.c_str());
          displayShowMessage("Sub-GHz", ok ? "Replayed" : "Not a valid RF file");
          delay(600);
          enterState(STATE_FILE_ACTION);
          break;
        }
        case 3: {
          bool ok = sdDeleteFile(path.c_str());
          displayShowMessage("SD Files", ok ? "Deleted" : "Delete failed");
          delay(600);
          enterState(STATE_FILE_MANAGER);
          break;
        }
        case 4: enterState(STATE_FILE_MANAGER); break;
      }
    }
    displayCheckIdle();
    return;
  }

  // ---- WiFi ----
  if (state == ST_WIFI_MENU) wifiHandleMenuEvent(evt);
  else if (state == ST_WIFI_SCAN) wifiHandleScanEvent(evt);
  else if (state == ST_WIFI_METER) wifiHandleMeterEvent(evt);
  else if (state == ST_WIFI_PACKET_MON) wifiHandlePacketMonitorEvent(evt);
  else if (state == ST_WIFI_DEAUTH) wifiHandleDeauthDetectEvent(evt);
  else if (state == ST_WIFI_DEAUTH_PICK) wifiHandleDeauthPickEvent(evt);
  else if (state == ST_WIFI_DEAUTH_RUN) wifiHandleDeauthRunEvent(evt);
  else if (state == ST_WIFI_BEACON_PICK) wifiHandleBeaconPickEvent(evt);
  else if (state == ST_WIFI_BEACON_RUN) wifiHandleBeaconRunEvent(evt);
  else if (state == ST_WIFI_CHANNEL_ANALYZER) wifiHandleChannelAnalyzerEvent(evt);
  else if (state == ST_WIFI_SNIFFER) wifiHandleSnifferEvent(evt);
  else if (state == ST_WIFI_CONNECT) wifiHandleConnectEvent(evt);
  else if (state == ST_WIFI_SCANHOSTS) wifiHandleScanHostsEvent(evt);
  else if (state == ST_WIREGUARD) wgHandleMenuEvent(evt);
  else if (state == ST_WIFI_EVIL_PORTAL) wifiHandleEvilPortalEvent(evt);
  else if (state == ST_WIFI_HANDSHAKE_MENU) wifiHandleHandshakeMenuEvent(evt);
  else if (state == ST_WIFI_HANDSHAKE_CAPTURE) wifiHandleHandshakeCaptureEvent(evt);
  else if (state == ST_WIFI_HANDSHAKE_RESULT) wifiHandleHandshakeResultEvent(evt);

  // ---- BLE ----
  if (state == ST_BLE_MENU) {
    if (evt == EVT_UP)   { bleMenu.up();   enterState(STATE_BLE_MENU); }
    if (evt == EVT_DOWN) { bleMenu.down(); enterState(STATE_BLE_MENU); }
    if (evt == EVT_OK) {
      switch (bleMenu.index()) {
        case 0: enterState(STATE_BLE_SCAN); break;
        case 1: enterState(STATE_BLE_AIRTAG); break;
        case 2: enterState(STATE_BLE_SKIMMER); break;
        case 3: enterState(STATE_BLE_SPAM); break;
      }
    }
  }
  if (state == ST_BLE_SCAN)    { if (evt == EVT_OK) enterState(STATE_BLE_SCAN); }
  if (state == ST_BLE_AIRTAG)  { if (evt == EVT_OK) enterState(STATE_BLE_AIRTAG); }
  if (state == ST_BLE_SKIMMER) { if (evt == EVT_OK) enterState(STATE_BLE_SKIMMER); }
  if (state == ST_BLE_SPAM)    { bleSpamHandleEvent(evt); }

  // ---- HID ----
  if (state == ST_HID_MENU) hidMenuEvent(evt);
  else if (state == ST_HID_SELECT_MODE) hidModeSelectEvent(evt);
  else if (state == ST_HID_KEYBOARD) hidKeyboardEvent(evt);
  else if (state == ST_HID_MOUSE) hidMouseEvent(evt);
  else if (state == ST_HID_SCRIPT_RUN) {
    hidScriptTick();

    static unsigned long lastBadUsbRedraw = 0;
    if (millis() - lastBadUsbRedraw > 200) {
      lastBadUsbRedraw = millis();
      drawBadUsbRunScreen();
    }

    if (!hidIsScriptRunning()) {
      String status = hidScriptGetStatus();
      displayShowMessage("BadUSB", status.c_str());
      delay(1200);
      hidSetMode(HID_MODE_OFF);
      enterState(STATE_HID_MENU);
    }
  }

  // ---- Games ----
  if (state == ST_GAMES_MENU) {
    if (evt == EVT_UP)   { gamesMenu.up();   enterState(STATE_GAMES_MENU); }
    if (evt == EVT_DOWN) { gamesMenu.down(); enterState(STATE_GAMES_MENU); }
    if (evt == EVT_OK) {
      switch (gamesMenu.index()) {
        case 0: enterState(STATE_GAME_SNAKE); break;
        case 1: enterState(STATE_GAME_TETRIS); break;
        case 2: enterState(STATE_GAME_PONG); break;
      }
    }
  }
  if (state == ST_GAME_SNAKE)  { snakeHandleInput(evt);  snakeUpdate();  }
  if (state == ST_GAME_TETRIS) { tetrisHandleInput(evt); tetrisUpdate(); }
  if (state == ST_GAME_PONG)   { pongHandleInput(evt);   pongUpdate();   }

  // ---- Settings ----
  if (state == ST_SETTINGS) settingsHandleInput(evt);

  // ---- GPS ----
  if (state == ST_GPS_MENU) gpsHandleMenuEvent(evt);
  else if (state == ST_GPS_LIVE) gpsHandleLiveEvent(evt);
  else if (state == ST_GPS_LOG) gpsHandleLogEvent(evt);
  else if (state == ST_GPS_WAYPOINTS) gpsHandleWaypointsEvent(evt);
  else if (state == ST_GPS_WAYPOINT_VIEW) gpsHandleWaypointViewEvent(evt);
  else if (state == ST_GPS_WARDRIVE) gpsHandleWardriveEvent(evt);

  // ---- LoRa ----
  if (state == ST_LORA_MENU) loraHandleMenuEvent(evt);
  else if (state == ST_LORA_CHAT) loraHandleChatEvent(evt, loraGotPacket, loraIncoming, loraIncRssi, loraIncSnr);
  else if (state == ST_LORA_MONITOR) loraHandleMonitorEvent(evt, loraGotPacket);
  else if (state == ST_LORA_SETTINGS) loraHandleSettingsEvent(evt);

  // ---- GPIO ----
  if (state == ST_GPIO_MENU) gpioHandleMenuEvent(evt);
  else if (state == ST_GPIO_DIO) gpioHandleDioEvent(evt);
  else if (state == ST_GPIO_EXPANDER) gpioHandleExpanderEvent(evt);
  else if (state == ST_GPIO_I2C_MENU) gpioHandleI2CMenuEvent(evt);
  else if (state == ST_GPIO_I2C_SCAN) gpioHandleI2CScanEvent(evt);
  else if (state == ST_GPIO_SPI_MENU) gpioHandleSPIMenuEvent(evt);
  else if (state == ST_GPIO_SPI_PROBE) gpioHandleSPIProbeEvent(evt);
  else if (state == ST_GPIO_UART_MENU) gpioHandleUARTMenuEvent(evt);
  else if (state == ST_GPIO_UART_ACTIVE) gpioHandleUARTActiveEvent(evt);
  else if (state == ST_GPIO_1WIRE) gpioHandle1WireEvent(evt);
  else if (state == ST_GPIO_2WIRE) gpioHandle2WireEvent(evt);
  else if (state == ST_GPIO_3WIRE) threewireHandleMenuEvent(evt);
  else if (state == ST_GPIO_NFC) gpioHandleNFCEvent(evt);
  else if (state == ST_GPIO_RFID) gpioHandleRFIDEvent(evt);
  else if (state == ST_GPIO_NRF) gpioHandleNRFEvent(evt);
  else if (state == ST_GPIO_CAN) gpioHandleCANEvent(evt);

  // ---- Web UI ----
  if (state == ST_WEB_UI) {
    webHandle();
    displayCheckIdle();
    return;
  }

  displayCheckIdle();
}