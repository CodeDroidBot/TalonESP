#include "gpio_module.h"
#include "config.h"
#include "display.h"
#include "buttons.h"
#include "menu.h"
#include "services/services.h"
#include "threewire/threewire_module.h"
#include <Wire.h>
#include <SPI.h>

extern void enterState(int stateId);

// ============================================================
//  Header pin table
// ============================================================
static const uint8_t HDR_PIN_COUNT = 10;
static const uint8_t gpioPins[HDR_PIN_COUNT] = {
  PIN_GPIO_IO1, PIN_GPIO_IO2, PIN_GPIO_IO3, PIN_GPIO_IO4, PIN_GPIO_IO5,
  PIN_GPIO_IO6, PIN_GPIO_IO7, PIN_GPIO_IO8, PIN_GPIO_IO9, PIN_GPIO_IO10
};
static const char* gpioPinNames[HDR_PIN_COUNT] = {
  "IO1", "IO2", "IO3", "IO4", "IO5", "IO6", "IO7", "IO8", "IO9", "IO10"
};

TwoWire gpioWire = TwoWire(1);
SPIClass gpioSPI(FSPI);

TwoWire& gpioGetWire() { return gpioWire; }
SPIClass& gpioGetSPI() { return gpioSPI; }

void gpioSleepBuses() {
  gpioWire.end();
  gpioSPI.end();
}

// ============================================================
//  Top-level menu — order MUST match case indices below
// ============================================================
static SimpleMenu gpioMenu({
  "Pin Control (Header)",     // 0
  "Pin Control (Expander)",   // 1
  "I2C",                      // 2
  "SPI",                      // 3
  "UART",                     // 4
  "1-Wire",                   // 5
  "2-Wire",                   // 6
  "3-Wire",                   // 7
  "NFC",                      // 8
  "RFID Reader",              // 9
  "nRF24 (2.4 GHz)",          // 10
  "CAN Bus"                   // 11
});

// ============================================================
//  Pin Control (Header) state
// ============================================================
static DioMode dioModes[HDR_PIN_COUNT];
static uint8_t dioSelected = 0;
static uint8_t dioPwmDuty[HDR_PIN_COUNT] = {128, 128, 128, 128, 128, 128, 128, 128, 128, 128};

static const char* dioModeNames[DIO_MODE_COUNT] = { "IN", "IN-PU", "OUT-0", "OUT-1", "PWM" };

static uint8_t dioPwmChannelFor(uint8_t idx) { return idx % 8; }

static void dioApplyPin(uint8_t idx) {
  uint8_t pin = gpioPins[idx];
  switch (dioModes[idx]) {
    case DIO_INPUT:
      ledcDetachPin(pin);
      pinMode(pin, INPUT);
      break;
    case DIO_INPUT_PULLUP:
      ledcDetachPin(pin);
      pinMode(pin, INPUT_PULLUP);
      break;
    case DIO_OUTPUT_LOW:
      ledcDetachPin(pin);
      pinMode(pin, OUTPUT);
      digitalWrite(pin, LOW);
      break;
    case DIO_OUTPUT_HIGH:
      ledcDetachPin(pin);
      pinMode(pin, OUTPUT);
      digitalWrite(pin, HIGH);
      break;
    case DIO_PWM: {
      int ch = dioPwmChannelFor(idx);
      ledcSetup(ch, 5000, 8);
      ledcAttachPin(pin, ch);
      ledcWrite(ch, dioPwmDuty[idx]);
      break;
    }
    default: break;
  }
}

void gpioModuleInit() {
  for (uint8_t i = 0; i < HDR_PIN_COUNT; i++) {
    dioModes[i] = DIO_INPUT;
    pinMode(gpioPins[i], INPUT);
  }
}

static void drawDioScreen() {
  tft.fillScreen(displayColorBg());
  displayDrawHeaderBar("GPIO - Pin Control");
  tft.setTextSize(1);
  int y = displayHeaderHeight() + 8;
  const int lineH = 16;
  for (uint8_t i = 0; i < HDR_PIN_COUNT; i++) {
    bool sel = (i == dioSelected);
    tft.setTextColor(sel ? TFT_YELLOW : displayColorFg(), displayColorBg());
    tft.setCursor(10, y);
    tft.printf("%s%-4s GPIO%-2d  %-5s",
               sel ? ">" : " ", gpioPinNames[i], gpioPins[i], dioModeNames[dioModes[i]]);

    if (dioModes[i] == DIO_INPUT || dioModes[i] == DIO_INPUT_PULLUP) {
      int lvl = digitalRead(gpioPins[i]);
      tft.printf("  =%d", lvl);
    } else if (dioModes[i] == DIO_PWM) {
      tft.printf("  %d%%", (dioPwmDuty[i] * 100) / 255);
    }
    y += lineH;
  }
  displayDrawFooterBar("UP/DN pin  L/R mode  OK act  BACK exit");
}

void gpioEnterDio() {
  dioSelected = 0;
  drawDioScreen();
}

void gpioHandleDioEvent(int evt) {
  // Early-return on EVT_NONE — otherwise the trailing redraw would run
  // on every loop iteration and cause visible flicker.
  if (evt == EVT_NONE) return;

  if (evt == EVT_UP)   { dioSelected = (dioSelected + HDR_PIN_COUNT - 1) % HDR_PIN_COUNT; drawDioScreen(); return; }
  if (evt == EVT_DOWN) { dioSelected = (dioSelected + 1) % HDR_PIN_COUNT; drawDioScreen(); return; }
  if (evt == EVT_LEFT || evt == EVT_RIGHT) {
    int m = (int)dioModes[dioSelected];
    m += (evt == EVT_RIGHT) ? 1 : -1;
    if (m < 0) m = DIO_MODE_COUNT - 1;
    if (m >= DIO_MODE_COUNT) m = 0;
    dioModes[dioSelected] = (DioMode)m;
    dioApplyPin(dioSelected);
    drawDioScreen();
    return;
  }
  if (evt == EVT_OK) {
    DioMode m = dioModes[dioSelected];
    if (m == DIO_OUTPUT_LOW)       { dioModes[dioSelected] = DIO_OUTPUT_HIGH; dioApplyPin(dioSelected); }
    else if (m == DIO_OUTPUT_HIGH) { dioModes[dioSelected] = DIO_OUTPUT_LOW;  dioApplyPin(dioSelected); }
    else if (m == DIO_PWM) {
      dioPwmDuty[dioSelected] = (dioPwmDuty[dioSelected] + 32) % 256;
      dioApplyPin(dioSelected);
    }
    drawDioScreen();
    return;
  }
}

// ============================================================
//  Pin Control (Expander) — MCP23017
// ============================================================
static const uint8_t MCP_I2C_ADDR = 0x20 + MCP23017_ADDR_OFFSET;

static const uint8_t REG_IODIRA = 0x00, REG_IODIRB = 0x01;
static const uint8_t REG_GPPUA  = 0x0C, REG_GPPUB  = 0x0D;
static const uint8_t REG_GPIOA  = 0x12, REG_GPIOB  = 0x13;

static uint8_t mcpReadReg(uint8_t reg) {
  Wire.beginTransmission(MCP_I2C_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom((int)MCP_I2C_ADDR, 1);
  return Wire.available() ? Wire.read() : 0xFF;
}

static void mcpWriteReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MCP_I2C_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static void mcpSetBit(uint8_t reg, uint8_t bit, bool val) {
  uint8_t v = mcpReadReg(reg);
  if (val) v |= (1 << bit); else v &= ~(1 << bit);
  mcpWriteReg(reg, v);
}

static bool mcpGetBit(uint8_t reg, uint8_t bit) {
  return (mcpReadReg(reg) >> bit) & 0x01;
}

struct ExpPin { uint8_t bank; uint8_t bit; const char* name; };
static const uint8_t EXP_PIN_COUNT = 10;
static const ExpPin expPins[EXP_PIN_COUNT] = {
  {0, 6, "A6"}, {0, 7, "A7"},
  {1, 0, "B0"}, {1, 1, "B1"}, {1, 2, "B2"}, {1, 3, "B3"},
  {1, 4, "B4"}, {1, 5, "B5"}, {1, 6, "B6"}, {1, 7, "B7"},
};

static ExpMode expModes[EXP_PIN_COUNT];
static uint8_t expSelected = 0;
static const char* expModeNames[EXP_MODE_COUNT] = { "IN", "IN-PU", "OUT-0", "OUT-1" };

static void expApplyPin(uint8_t idx) {
  const ExpPin& p = expPins[idx];
  uint8_t iodirReg = (p.bank == 0) ? REG_IODIRA : REG_IODIRB;
  uint8_t gppuReg  = (p.bank == 0) ? REG_GPPUA  : REG_GPPUB;
  uint8_t gpioReg  = (p.bank == 0) ? REG_GPIOA  : REG_GPIOB;

  switch (expModes[idx]) {
    case EXP_INPUT:         mcpSetBit(iodirReg, p.bit, true);  mcpSetBit(gppuReg, p.bit, false); break;
    case EXP_INPUT_PULLUP:  mcpSetBit(iodirReg, p.bit, true);  mcpSetBit(gppuReg, p.bit, true);  break;
    case EXP_OUTPUT_LOW:    mcpSetBit(iodirReg, p.bit, false); mcpSetBit(gpioReg, p.bit, false); break;
    case EXP_OUTPUT_HIGH:   mcpSetBit(iodirReg, p.bit, false); mcpSetBit(gpioReg, p.bit, true);  break;
    default: break;
  }
}

static void drawExpanderScreen() {
  tft.fillScreen(displayColorBg());
  displayDrawHeaderBar("GPIO - Expander (MCP23017)");
  tft.setTextSize(1);
  int y = displayHeaderHeight() + 8;
  const int lineH = 16;
  for (uint8_t i = 0; i < EXP_PIN_COUNT; i++) {
    bool sel = (i == expSelected);
    const ExpPin& p = expPins[i];
    tft.setTextColor(sel ? TFT_YELLOW : displayColorFg(), displayColorBg());
    tft.setCursor(10, y);
    tft.printf("%s%-3s  %-5s", sel ? ">" : " ", p.name, expModeNames[expModes[i]]);
    if (expModes[i] == EXP_INPUT || expModes[i] == EXP_INPUT_PULLUP) {
      uint8_t gpioReg = (p.bank == 0) ? REG_GPIOA : REG_GPIOB;
      tft.printf("  =%d", mcpGetBit(gpioReg, p.bit) ? 1 : 0);
    }
    y += lineH;
  }
  displayDrawFooterBar("UP/DN pin  L/R mode  OK toggle  BACK exit");
}

void gpioEnterExpander() {
  static bool initedOnce = false;
  if (!initedOnce) {
    for (uint8_t i = 0; i < EXP_PIN_COUNT; i++) expModes[i] = EXP_INPUT;
    initedOnce = true;
  }
  for (uint8_t i = 0; i < EXP_PIN_COUNT; i++) expApplyPin(i);
  expSelected = 0;
  drawExpanderScreen();
}

void gpioHandleExpanderEvent(int evt) {
  // Same flicker fix as the header pin-control screen.
  if (evt == EVT_NONE) return;

  if (evt == EVT_UP)   { expSelected = (expSelected + EXP_PIN_COUNT - 1) % EXP_PIN_COUNT; drawExpanderScreen(); return; }
  if (evt == EVT_DOWN) { expSelected = (expSelected + 1) % EXP_PIN_COUNT; drawExpanderScreen(); return; }
  if (evt == EVT_LEFT || evt == EVT_RIGHT) {
    int m = (int)expModes[expSelected];
    m += (evt == EVT_RIGHT) ? 1 : -1;
    if (m < 0) m = EXP_MODE_COUNT - 1;
    if (m >= EXP_MODE_COUNT) m = 0;
    expModes[expSelected] = (ExpMode)m;
    expApplyPin(expSelected);
    drawExpanderScreen();
    return;
  }
  if (evt == EVT_OK) {
    ExpMode m = expModes[expSelected];
    if (m == EXP_OUTPUT_LOW)       { expModes[expSelected] = EXP_OUTPUT_HIGH; expApplyPin(expSelected); }
    else if (m == EXP_OUTPUT_HIGH) { expModes[expSelected] = EXP_OUTPUT_LOW;  expApplyPin(expSelected); }
    drawExpanderScreen();
    return;
  }
}

// ============================================================
//  Top-level GPIO menu
// ============================================================
void gpioEnterMenu() {
  gpioModuleSleep();
  displayShowMenu("GPIO", gpioMenu.items(), gpioMenu.index());
}

void gpioHandleMenuEvent(int evt) {
  if (evt == EVT_NONE) return;

  if (evt == EVT_UP)   { gpioMenu.up();   gpioEnterMenu(); return; }
  if (evt == EVT_DOWN) { gpioMenu.down(); gpioEnterMenu(); return; }
  if (evt == EVT_OK) {
    switch (gpioMenu.index()) {
      case 0:  enterState(STATE_GPIO_DIO);        break;
      case 1:  enterState(STATE_GPIO_EXPANDER);   break;
      case 2:  enterState(STATE_GPIO_I2C_MENU);   break;
      case 3:  enterState(STATE_GPIO_SPI_MENU);   break;
      case 4:  enterState(STATE_GPIO_UART_MENU);  break;
      case 5:  enterState(STATE_GPIO_1WIRE);      break;
      case 6:  enterState(STATE_GPIO_2WIRE);      break;
      case 7:  enterState(STATE_GPIO_3WIRE);      break;
      case 8:  enterState(STATE_GPIO_NFC);        break;
      case 9:  enterState(STATE_GPIO_RFID);       break;
      case 10: enterState(STATE_GPIO_NRF);        break;
      case 11: enterState(STATE_GPIO_CAN);        break;
    }
  }
}

// ============================================================
//  1-Wire helpers (bit-bang)
// ============================================================
static uint8_t owPin() { return gpioPins[4]; } // IO5

static bool owReset() {
  uint8_t pin = owPin();
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  delayMicroseconds(480);
  pinMode(pin, INPUT_PULLUP);
  delayMicroseconds(70);
  bool presence = (digitalRead(pin) == LOW);
  delayMicroseconds(410);
  return presence;
}

static void owWriteBit(bool bit) {
  uint8_t pin = owPin();
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  delayMicroseconds(bit ? 6 : 60);
  pinMode(pin, INPUT_PULLUP);
  delayMicroseconds(bit ? 64 : 10);
}

static bool owReadBit() {
  uint8_t pin = owPin();
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  delayMicroseconds(6);
  pinMode(pin, INPUT_PULLUP);
  delayMicroseconds(9);
  bool bit = digitalRead(pin);
  delayMicroseconds(55);
  return bit;
}

static void owWriteByte(uint8_t b) {
  for (int i = 0; i < 8; i++) { owWriteBit(b & 0x01); b >>= 1; }
}

static uint8_t owReadByte() {
  uint8_t b = 0;
  for (int i = 0; i < 8; i++) b |= (owReadBit() << i);
  return b;
}

// ============================================================
//  1-Wire detection (used by Web UI)
// ============================================================
bool oneWireDetect(String& outRom) {
  uint8_t pin = owPin();
  noInterrupts();
  bool present = owReset();
  uint8_t rom[8] = {0};
  if (present) {
    owWriteByte(0x33);
    for (int i = 0; i < 8; i++) rom[i] = owReadByte();
  }
  interrupts();
  if (!present) return false;
  outRom = "";
  for (int i = 0; i < 8; i++) {
    if (rom[i] < 0x10) outRom += "0";
    outRom += String(rom[i], HEX);
  }
  return true;
}

// ============================================================
//  1-Wire screen
// ============================================================
void gpioEnter1Wire() {
  tft.fillScreen(displayColorBg());
  displayDrawHeaderBar("1-Wire (IO5)");
  tft.setTextSize(1);
  tft.setTextColor(displayColorFg(), displayColorBg());
  tft.setCursor(10, displayHeaderHeight() + 10);

  noInterrupts();
  bool present = owReset();
  uint8_t rom[8] = {0};
  if (present) {
    owWriteByte(0x33);
    for (int i = 0; i < 8; i++) rom[i] = owReadByte();
  }
  interrupts();

  if (!present) {
    tft.println("No device detected.");
    tft.println("Connect iButton/1-Wire");
    tft.println("data line to IO5.");
  } else {
    tft.println("Device present!");
    tft.print("ROM: ");
    for (int i = 0; i < 8; i++) tft.printf("%02X", rom[i]);
    tft.println();
    tft.printf("Family: 0x%02X\n", rom[0]);
  }
  displayDrawFooterBar("OK rescan  BACK exit");
}

void gpioHandle1WireEvent(int evt) {
  if (evt == EVT_OK) gpioEnter1Wire();
}

// ============================================================
//  2-Wire stub
// ============================================================
void gpioEnter2Wire() {
  displayShowMessage("2-Wire (Smart Card / I2C)",
                     "2-Wire protocol is not yet implemented.\n"
                     "This mode is reserved for smart card\n"
                     "communication (ISO 7816) or I2C sniffing.\n"
                     "Press BACK to exit.");
}

void gpioHandle2WireEvent(int evt) {
  (void)evt;
}

// ============================================================
//  3-Wire delegation
// ============================================================
void gpioEnter3Wire() {
  threewireEnterMenu();
}

void gpioHandle3WireEvent(int evt) {
  threewireHandleMenuEvent(evt);
}

// ============================================================
//  Low-power teardown
// ============================================================
static void gpioSleepHeaderPins() {
  for (uint8_t i = 0; i < HDR_PIN_COUNT; i++) {
    ledcDetachPin(gpioPins[i]);
    pinMode(gpioPins[i], INPUT);
    dioModes[i] = DIO_INPUT;
  }
}

static void gpioSleepExpanderPins() {
  for (uint8_t i = 0; i < EXP_PIN_COUNT; i++) {
    expModes[i] = EXP_INPUT;
    expApplyPin(i);
  }
}

void gpioModuleSleep() {
  gpioSleepBuses();
  gpioUartBridgeStop();
  gpioSleepHeaderPins();
  gpioSleepExpanderPins();
  I2C_SERVICE.end();
  SPI_SERVICE.end();
  UART_SERVICE.end();
}

// ============================================================
//  Web UI bridge functions
// ============================================================
std::vector<ExpPinState> gpioExpanderRead() {
  std::vector<ExpPinState> out;
  for (uint8_t i = 0; i < EXP_PIN_COUNT; i++) {
    const ExpPin& p = expPins[i];
    uint8_t gpioReg = (p.bank == 0) ? REG_GPIOA : REG_GPIOB;
    int val = mcpGetBit(gpioReg, p.bit) ? 1 : 0;
    out.push_back({String(p.name), val});
  }
  return out;
}

void gpioSetDioMode(uint8_t pinIndex, DioMode mode) {
  if (pinIndex >= HDR_PIN_COUNT) return;
  dioModes[pinIndex] = mode;
  dioApplyPin(pinIndex);
}

void gpioSetDioPwmDuty(uint8_t pinIndex, uint8_t duty) {
  if (pinIndex >= HDR_PIN_COUNT) return;
  if (dioModes[pinIndex] != DIO_PWM) return;
  dioPwmDuty[pinIndex] = duty;
  dioApplyPin(pinIndex);
}

void gpioSetDioOutput(uint8_t pinIndex, bool high) {
  if (pinIndex >= HDR_PIN_COUNT) return;
  if (dioModes[pinIndex] != DIO_OUTPUT_LOW && dioModes[pinIndex] != DIO_OUTPUT_HIGH) return;
  dioModes[pinIndex] = high ? DIO_OUTPUT_HIGH : DIO_OUTPUT_LOW;
  dioApplyPin(pinIndex);
}

void gpioSetExpanderMode(uint8_t pinIndex, ExpMode mode) {
  if (pinIndex >= EXP_PIN_COUNT) return;
  expModes[pinIndex] = mode;
  expApplyPin(pinIndex);
}

void gpioSetExpanderOutput(uint8_t pinIndex, bool high) {
  if (pinIndex >= EXP_PIN_COUNT) return;
  if (expModes[pinIndex] != EXP_OUTPUT_LOW && expModes[pinIndex] != EXP_OUTPUT_HIGH) return;
  expModes[pinIndex] = high ? EXP_OUTPUT_HIGH : EXP_OUTPUT_LOW;
  expApplyPin(pinIndex);
}

// ============================================================
//  Cleanup
// ============================================================
void gpioCleanup() {
  gpioModuleSleep();
}