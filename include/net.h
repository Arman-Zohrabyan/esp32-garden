#pragma once

#include <Arduino.h>

// Wi-Fi: connecting to the router, our own access point, air scan, mDNS.
//
// This is where all the logic lives that's needed BEFORE the board gets onto the network:
// the setup portal, auto-connect to the saved network, garden.local.
// Without it the device can't be set up, so touch with care.

// Connection status for display on the web page.
extern String staStatus;

// Scan results cache: -2 = scan not started yet, -1 = in progress, >=0 = number of networks.
extern int scanCount;

// Whether our own access point is enabled.
extern bool apOn;

// Escaping text for insertion into HTML. The SSID comes from the air and may
// contain ' < > & — without escaping it breaks the page layout and
// opens up injection via someone else's network name.
String htmlEscape(const String& s);

// Asynchronous scan: startScan() starts it, pollScan() picks up the result
// from loop(). A synchronous scan would block the page for seconds.
void startScan();
void pollScan();

// Shared connect-wait loop: calls WiFi.begin and waits up to the timeout.
// Used by both the portal and the active cycle — to avoid duplication.
bool waitConnect(const String& ssid, const String& pass, unsigned long timeoutMs);

// Connect to the router + bring up mDNS (garden.local) + fill in staStatus.
bool connectTo(const String& ssid, const String& pass);

// Auto-connect to the network saved in flash (NVS namespace "wifi").
// Returns false if there's no saved network or connecting failed.
bool connectSaved();

// Save the network to flash after a successful connection.
void saveWifiCredentials(const String& ssid, const String& pass);

// Forget the saved network — the board will return to portal mode.
void forgetWifi();

// Bring up our own ESP32-Garden access point.
void startAP();

// Turns off the AP when the board is connected to the router (less heat, one radio
// instead of two). If the connection drops — brings it back up as a fallback entry point.
// Call from loop().
void manageAP();
