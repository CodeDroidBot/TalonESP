// web_ui.h
#pragma once
#include <Arduino.h>

void webStartServer();   // called when entering STATE_WEB_UI
void webStopServer();    // called when leaving (optional)
String webGetLocalIP();  // returns the IP address (STA or AP) for display
void webHandle();        // call in loop() to keep the server responsive