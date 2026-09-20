// wifi_module.h
#pragma once
#include <Arduino.h>
#include <vector>

// ---- Types (unchanged) ----
struct WifiNet {
  String ssid;
  String bssid;
  int rssi;
  int channel;
  String enc;
};

struct WifiPacketCounts {
  uint32_t mgmt;
  uint32_t beacon;
  uint32_t probeReq;
  uint32_t probeResp;
  uint32_t deauth;
  uint32_t disassoc;
  uint32_t ctrl;
  uint32_t data;
};

// ---- Core functions (unchanged) ----
void wifiInit();
std::vector<WifiNet> wifiScanRaw();
std::vector<String> wifiScanNetworks();
void wifiScanStart();
bool wifiScanIsComplete();
bool wifiScanHasFailed();
std::vector<WifiNet> wifiScanFinish();
String wifiGetStatusLine();

void wifiStartPacketMonitor();
void wifiStopPacketMonitor();
void wifiPacketMonitorHopChannel();
void wifiPacketMonitorSetChannel(uint8_t channel);
uint8_t wifiPacketMonitorGetChannel();
WifiPacketCounts wifiGetPacketCounts();
void wifiResetPacketCounts();
uint32_t wifiGetDeauthCount();

void wifiDeauthSetTarget(const String& ssid, const String& bssidStr, uint8_t channel);
void wifiDeauthStart();
void wifiDeauthStop();
bool wifiDeauthIsRunning();
uint32_t wifiDeauthFramesSent();
String wifiDeauthGetTargetSsid();
String wifiDeauthGetTargetBssid();
uint8_t wifiDeauthGetChannel();
void wifiDeauthTick();

void wifiBeaconSetChannel(uint8_t channel);
void wifiBeaconStart();
void wifiBeaconStop();
bool wifiBeaconIsRunning();
uint32_t wifiBeaconFramesSent();
uint8_t wifiBeaconGetChannel();
void wifiBeaconTick();

void wifiEvilPortalStart();
void wifiEvilPortalStop();
bool wifiEvilPortalIsRunning();
void wifiEvilPortalTick();

// ---- UI handlers (unchanged) ----
void wifiEnterMenu();
void wifiHandleMenuEvent(int evt);
void wifiEnterScan();
void wifiHandleScanEvent(int evt);
void wifiEnterMeter();
void wifiHandleMeterEvent(int evt);
void wifiEnterPacketMonitor();
void wifiHandlePacketMonitorEvent(int evt);
void wifiEnterDeauthDetect();
void wifiHandleDeauthDetectEvent(int evt);
void wifiEnterDeauthPick();
void wifiHandleDeauthPickEvent(int evt);
void wifiEnterDeauthRun();
void wifiHandleDeauthRunEvent(int evt);
void wifiEnterBeaconPick();
void wifiHandleBeaconPickEvent(int evt);
void wifiEnterBeaconRun();
void wifiHandleBeaconRunEvent(int evt);
void wifiEnterChannelAnalyzer();
void wifiHandleChannelAnalyzerEvent(int evt);
void wifiEnterSniffer();
void wifiHandleSnifferEvent(int evt);
void wifiEnterConnect();
void wifiHandleConnectEvent(int evt);
void wifiEnterScanHosts();
void wifiHandleScanHostsEvent(int evt);
void wifiEnterEvilPortal();
void wifiHandleEvilPortalEvent(int evt);

// ---- Channel Analyzer (unchanged) ----
void wifiChannelAnalyzerStart();
void wifiChannelAnalyzerStop();
void wifiChannelAnalyzerTick();
uint16_t wifiChannelAnalyzerGetActivity(uint8_t channel1to13);
uint8_t wifiChannelAnalyzerGetCurrentChannel();

// ---- Sniffer (unchanged) ----
void wifiSnifferStart();
void wifiSnifferStop();
void wifiSnifferSetChannel(uint8_t ch);
void wifiSnifferHopChannel();
uint8_t wifiSnifferGetChannel();
uint32_t wifiSnifferGetLoggedCount();
uint32_t wifiSnifferGetDroppedCount();
void wifiSnifferTick();

// ---- STA connect (unchanged) ----
std::vector<String> wifiListSavedNetworks();
bool wifiConnectSaved(int index, unsigned long timeoutMs = 8000);
bool wifiIsConnected();
String wifiGetLocalIpStr();

// ---- Scan Hosts (unchanged) ----
struct HostHit {
  uint32_t ip;
  bool alive;
};
std::vector<HostHit> wifiScanHosts(uint16_t port = 80, uint16_t perBatchTimeoutMs = 300);

// ================================================================
//  NEW: Handshake Capture (using promiscuous mode)
// ================================================================
void wifiHandshakeStartCapture(const String& bssidStr, uint8_t channel, const String& ssid = "");
void wifiHandshakeStopCapture();
bool wifiHandshakeIsRunning();
bool wifiHandshakeIsComplete();
String wifiHandshakeGetResultPath();   // "/handshakes/capture.pcap"
String wifiHandshakeGetStatus();
void wifiHandshakeTick();

// UI entry points (matching main.cpp)
void wifiEnterHandshakeMenu();
void wifiHandleHandshakeMenuEvent(int evt);
void wifiEnterHandshakeCapture();
void wifiHandleHandshakeCaptureEvent(int evt);
void wifiEnterHandshakeResult();
void wifiHandleHandshakeResultEvent(int evt);