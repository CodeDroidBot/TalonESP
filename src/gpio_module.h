#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <vector>

// ============================================================
//  GPIO / Bus-Pirate style multi-protocol app
// ============================================================

// ---- DIO modes ----
enum DioMode {
  DIO_INPUT,
  DIO_INPUT_PULLUP,
  DIO_OUTPUT_LOW,
  DIO_OUTPUT_HIGH,
  DIO_PWM
};
const int DIO_MODE_COUNT = 5;   // for array sizing

// ---- Expander modes ----
enum ExpMode {
  EXP_INPUT,
  EXP_INPUT_PULLUP,
  EXP_OUTPUT_LOW,
  EXP_OUTPUT_HIGH
};
const int EXP_MODE_COUNT = 4;

// ---- lifecycle ----
void gpioModuleInit();
TwoWire&   gpioGetWire();
SPIClass&  gpioGetSPI();
void gpioSleepBuses();
void gpioModuleSleep();

// ---- top-level menu ----
void gpioEnterMenu();
void gpioHandleMenuEvent(int evt);

// ---- Pin Control (DIO) ----
void gpioEnterDio();
void gpioHandleDioEvent(int evt);

// ---- Pin Control (Expander) ----
void gpioEnterExpander();
void gpioHandleExpanderEvent(int evt);

// ---- I2C, SPI, UART, 1-Wire, NFC ----
void gpioEnterI2CMenu();
void gpioHandleI2CMenuEvent(int evt);
void gpioEnterI2CScan();
void gpioHandleI2CScanEvent(int evt);

void gpioEnterSPIMenu();
void gpioHandleSPIMenuEvent(int evt);
void gpioEnterSPIProbe();
void gpioHandleSPIProbeEvent(int evt);

void gpioEnterUARTMenu();
void gpioHandleUARTMenuEvent(int evt);
void gpioEnterUARTActive();
void gpioHandleUARTActiveEvent(int evt);
void gpioUARTBridgeTick();
void gpioUartBridgeStop();

void gpioEnter1Wire();
void gpioHandle1WireEvent(int evt);

void gpioEnter2Wire();
void gpioHandle2WireEvent(int evt);
void gpioEnter3Wire();
void gpioHandle3WireEvent(int evt);

void gpioEnterNFC();
void gpioHandleNFCEvent(int evt);

// ---- GPIO control functions for Web UI ----
void gpioSetDioMode(uint8_t pinIndex, DioMode mode);
void gpioSetDioPwmDuty(uint8_t pinIndex, uint8_t duty);
void gpioSetDioOutput(uint8_t pinIndex, bool high);
void gpioSetExpanderMode(uint8_t pinIndex, ExpMode mode);
void gpioSetExpanderOutput(uint8_t pinIndex, bool high);

struct ExpPinState { String name; int value; };
std::vector<ExpPinState> gpioExpanderRead();

// ---- 1-Wire detection ----
bool oneWireDetect(String& outRom);

// I2C advanced
void i2cEnterRead();
void i2cEnterWrite();
void i2cEnterWriteRead();
void i2cEnterSpeed();
void i2cEnterSlave();

// SPI advanced
void spiEnterTransfer();
void spiEnterMode();
void spiEnterFreq();
void spiEnterSlave();

// UART advanced
void uartEnterTerminal();
void uartEnterSettings();

// 3-Wire
void threewireEnterMenu();
void threewireEnterRead();
void threewireEnterWrite();
void threewireEnterErase();

// nRF24 (2.4 GHz)
void gpioEnterNRF();
void gpioHandleNRFEvent(int evt);

// CAN bus
void gpioEnterCAN();
void gpioHandleCANEvent(int evt);