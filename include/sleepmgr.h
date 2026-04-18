#pragma once

#include <Arduino.h>

// Board identification, reboot diagnostics, and entering deep sleep.
//
// The file is named sleepmgr, not sleep: xtensa newlib has a system <sleep.h>,
// and "sleep.h" would cause a hard-to-spot build conflict.

// Board chipId — the station MAC without separators, 12 hex characters.
// The ESP32 has no separate chip ID (esptool literally replies to the request with
// "ESP32 has no Chip ID. Reading MAC instead"), so the identifier
// is the MAC from eFuse: it's burned in at the factory and immutable.
// Example: MAC 8C:94:DF:8B:7E:F8 -> "8C94DF8B7EF8".
String chipId();

// Reason for the last reboot as a string for the log:
// DEEPSLEEP (normal wake-up), PANIC (crash), BROWNOUT (power
// sag), WDT (hang), POWERON, EXT, SW, OTHER.
// BROWNOUT on battery is a direct sign that power is insufficient.
const char* resetReasonStr();

// Whether the last reboot was a failure rather than a normal event.
// true for BROWNOUT (power sag), PANIC (crash) and WDT (hang).
// Needed so such boots go to the log at error level: otherwise they're lost
// among routine timer wake-ups.
bool bootReasonIsFailure();

// Enter deep sleep for the given number of seconds. Does not return: after sleep
// the board boots again, from scratch through setup().
//
// Before sleep it switches GPIO25 (sensor power), GPIO26 (voltage divider), GPIO27
// (pump) and GPIO33 (pump cell voltage divider) to input without pulls and
// holds them that way for the entire sleep. Any entry into sleep must go
// only through this function, otherwise the voltage divider or sensor will stay powered.
void goToSleep(uint32_t seconds);

// Release the GPIO25/26/27/33 hold left over from goToSleep(), and immediately
// set the pump (GPIO27) to LOW. Call at the very start of setup(), before
// soilBegin()/batteryBegin().
void sleepPinsRelease();

// Whether the BOOT button is pressed right now. Used as an emergency stop
// on wake-up — the only way to get the board out of the sleep cycle
// when the server is unavailable.
bool bootButtonPressed();

// ---------- active cycle indication ----------
//
// Blinks the blue LED once a second for the entire time awake: it shows
// that the board isn't asleep but working. Without it the active cycle looks from outside
// exactly like deep sleep — the LED is off in both cases.
//
// Done with a hardware timer, not millis() in a loop: soil and battery readings
// block execution for tens of seconds (delay inside the window), and
// software blinking would simply stall there. The timer runs regardless of
// what the main thread is busy with.
void blinkStart();

// Turn off the LED and stop the timer. Must be called before entering
// sleep: otherwise the interrupt would stay attached to the pin.
void blinkStop();
