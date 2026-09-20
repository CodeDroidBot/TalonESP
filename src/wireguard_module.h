#pragma once
#include <Arduino.h>

// WireGuard VPN client.
//
// Requires an active WiFi STA connection first (see wifi_module.h:
// wifiConnectSaved / the "Connect (saved)" screen) - WireGuard tunnels
// over an existing WiFi link, it doesn't provide WiFi itself.
//
// Config is read from SD as a standard wg-quick .conf file at
// /wireguard.conf, e.g.:
//
//   [Interface]
//   PrivateKey = <your device's private key>
//   Address = 10.0.0.2/24
//
//   [Peer]
//   PublicKey = <server's public key>
//   Endpoint = vpn.example.com:51820
//   AllowedIPs = 0.0.0.0/0
//
// There's no on-device text entry for base64 keys in this UI, so the
// file is provisioned by editing the SD card from a computer (the same
// pattern already used for /wifi_creds.csv). On the VPN menu screen,
// LEFT re-reads /wireguard.conf from SD, so an edited file can be picked
// up without power-cycling the device.
//
// Library note (github.com/ciniml/WireGuard-ESP32-Arduino, which this
// fork tracks): it supports exactly one peer, no PresharedKey, no
// PersistentKeepalive, and doesn't act on DNS/AllowedIPs the way
// wg-quick does on a real OS - it just brings up a single default
// tunnel over lwIP. A .conf written for a "real" WireGuard client may
// have all of those fields; this module parses what it can, uses the
// Address CIDR to set the interface's netmask, and reports anything it
// had to ignore via wgGetConfigWarnings() so that's visible instead of
// silently different behavior.

bool wgLoadConfigFromSD();   // parses /wireguard.conf; false if missing/invalid
bool wgStart();              // brings the tunnel up using the loaded config
void wgStop();
bool wgIsUp();
String wgGetLocalTunnelIp();
String wgGetSubnetCidr();    // e.g. "10.0.0.2/24" - the Address line as configured
String wgGetEndpoint();
String wgGetLastError();

// Non-fatal notes about .conf fields this library can't act on (e.g.
// PresharedKey, PersistentKeepalive, DNS - see wgLoadConfigFromSD() in
// the .cpp for why). Empty string if the config had nothing to flag.
String wgGetConfigWarnings();

void wgEnterMenu();
void wgHandleMenuEvent(int evt);