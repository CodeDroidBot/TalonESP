#pragma once
#include <Arduino.h>

// ============================================================
//  CAN bus module — supports BOTH:
//
//   1. ESP32-S3 built-in TWAI controller (needs external 3.3V
//      CAN transceiver like SN65HVD230, TJA1050-T/3, MCP2551)
//      Wiring: IO1 = TX, IO2 = RX
//
//   2. MCP2515 SPI CAN controller (with on-board TJA1050)
//      Wiring (shares GPIO SPI bus):
//        IO1 = SCK, IO2 = MOSI, IO3 = MISO, IO4 = CS
//
//  Screen flow (single GPIO state, STATE_GPIO_CAN):
//    1) Bus-select submenu: TWAI (on-chip) vs MCP2515 (SPI)
//    2) Mode submenu: Sniffer, OBD-II Query, Send Frame
// ============================================================

void gpioEnterCAN();
void gpioHandleCANEvent(int evt);
bool canIsInitialized();
void canDeinit();