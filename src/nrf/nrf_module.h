#pragma once
#include <Arduino.h>

// ============================================================
//  nRF24L01+ 2.4 GHz module — scanner, sniffer, channel analyzer
//
//  Uses the RF24 library by TMRh20.
//
//  Wiring (uses the dedicated GPIO SPI bus from gpioGetSPI()):
//    IO1 = SCK
//    IO2 = MOSI
//    IO3 = MISO
//    IO4 = CE
//    IO5 = CSN
//
//  IMPORTANT: The nRF24L01 requires a stable 3.3V supply with a
//  10uF+ capacitor across VCC/GND. The PA+LNA variants draw
//  100mA+ during TX and will reset or fail init without it.
// ============================================================

void gpioEnterNRF();
void gpioHandleNRFEvent(int evt);
bool nrfIsInitialized();
void nrfDeinit();