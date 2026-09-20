#pragma once
#include <Arduino.h>
#include <vector>

struct GpsFix {
  bool valid;          // true if we have a recent (<5s old) location fix
  double lat;
  double lon;
  double altitudeM;
  double speedKmh;
  double courseDeg;    // heading over ground, 0 = North
  int satellites;
  double hdop;
  uint8_t hour, minute, second; // UTC
  uint8_t day, month;
  uint16_t year;
};

struct GpsWaypoint {
  String name;
  double lat;
  double lon;
  double altitudeM;
};

void gpsInit();
bool gpsDetect(unsigned long timeoutMs = 2000);
// Drains the GPS UART into the NMEA parser. Call every loop() iteration
// regardless of which screen is active, or characters get dropped.
void gpsUpdate();

bool gpsHasFix();
GpsFix gpsGetFix();

// Distance (meters, great-circle) and initial bearing (degrees, 0=North)
// from the current fix to an arbitrary point. Returns false if there's no
// fix yet.
bool gpsBearingTo(double lat, double lon, double& outDistanceM, double& outBearingDeg);

// ---- Track logging to SD (/gps_track.csv: time,lat,lon,alt,speed) ----
void gpsLogStart();
void gpsLogStop();
bool gpsLogIsRunning();
// Call every loop() iteration while a logging screen may be active (or
// always, if background logging is desired) - appends a row roughly every
// LOG_INTERVAL_MS while logging is on and a fix is available.
void gpsLogTick();
uint32_t gpsLogPointCount();

// ---- Waypoints (persisted to SD as /gps_waypoints.csv) ----
bool gpsWaypointSave(const String& name);
std::vector<GpsWaypoint> gpsWaypointList();
bool gpsWaypointDelete(int index);

void gpsEnterMenu();              // shows the GPS submenu (called from main)
void gpsHandleMenuEvent(int evt);

void gpsEnterLive();
void gpsHandleLiveEvent(int evt);

void gpsEnterLog();
void gpsHandleLogEvent(int evt);

void gpsEnterWaypoints();
void gpsHandleWaypointsEvent(int evt);

void gpsEnterWaypointView();
void gpsHandleWaypointViewEvent(int evt);
void gpsSetEnabled(bool enable);

// ---- Wardriving (GPS-tagged WiFi AP logging, WiGLE CSV format) ----
// Passive scan only (no deauth/injection). Appends unique BSSIDs seen this
// session, with current GPS fix, to /wardrive.csv on the SD card.
// Call gpsWardriveTick() every loop() iteration (like gpsLogTick()) so
// logging continues in the background once started, even from other menus.
void gpsWardriveTick();
void gpsEnterWardrive();
void gpsHandleWardriveEvent(int evt);
// Add to gps_module.h
void gpsWardriveStart();
void gpsWardriveStop();
bool gpsWardriveIsRunning();
uint32_t gpsWardriveGetNetCount();