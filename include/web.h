#pragma once

#include <WebServer.h>

// The board's local web interface — Wi-Fi setup portal and dashboard.
//
// This is the only way to activate the device (isActive false -> true),
// so in inactive mode the page must remain reachable:
// a sleeping board won't serve it. That's also why inactive mode doesn't sleep.

extern WebServer server;

// Registers all routes and starts the server. Call from setup().
void webBegin();

// Handles incoming requests. Call from loop().
void webLoop();
