#pragma once
#include <Arduino.h>

// Use the same struct as UartService
#include "services/UartService.h"   // for UartConfig

extern UartConfig uartConfig;

void gpioEnterUARTMenu();
void gpioHandleUARTMenuEvent(int evt);
void gpioEnterUARTActive();
void gpioHandleUARTActiveEvent(int evt);
void gpioUARTBridgeTick();
void gpioUartBridgeStop();
void uartEnterTerminal();
void uartEnterSettings();
String uartSendCommand(const String& cmd, unsigned long timeoutMs = 2000);