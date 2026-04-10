#pragma once

#include <Arduino.h>

// Li-ion battery voltage and charge reading via a voltage divider on GPIO34.
//
// Why GPIO34 specifically: the ESP32 has two ADCs, and ADC2 shares resources with
// the radio — with Wi-Fi on, reading from it returns an error.
// That's the architecture, not a bug. That leaves ADC1 (GPIO32-39), and of those 34/35/36/39
// are also input-only, which guards against accidentally configuring the pin as an output.

// Reading result. voltage — at the battery (already corrected for the voltage divider),
// percent — from the Li-ion discharge curve, not a linear formula.
struct BatteryReading {
  float voltage = 0.0f;
  int   percent = -1;      // -1 = not measured yet
  int   mvOnPin = 0;       // voltage at the pin itself, for diagnostics
};

// Pin and attenuation setup, plus enabling the voltage divider (GPIO26 -> LOW).
// Call in setup() before the first reading. The voltage divider stays on until
// batteryDividerOff() — goToSleep() calls it.
void batteryBegin();

// Turn off the voltage divider: GPIO26 to input without pulls, zero current through the voltage divider.
void batteryDividerOff();

// Battery voltage in volts. Averages 32 samples: a single
// reading is noisy, and the switched sampling capacitor at the ADC input
// "bites off" charge, so the first samples come out low.
float batteryVoltage();

// Percent from the Li-ion discharge curve. A linear formula doesn't work here:
// the 3.7-3.9 V plateau holds ~60% of capacity, while the last 10% drops
// from 3.5 to 3.0 V within minutes. A linear conversion would show 68% where
// it's actually 50%, and "zero" would hit suddenly.
int batteryPercent(float volts);

// Full reading: voltage + percent + smoothing.
// Smoothing keeps the percent from flickering back and forth by one.
BatteryReading batteryRead();

// A window of readings with pauses — for the active cycle. Blocking: takes
// (count-1) * gapMs milliseconds, so it has no place in loop().
//
// Why a window when batteryRead() already averages 32 samples: those samples
// run back to back within milliseconds and only suppress ADC noise. Li-ion voltage
// "breathes" under load — it sags on a current pulse (radio, sensor)
// and recovers within seconds. A stretched window catches exactly that and
// gives the resting voltage instead of a random point in the sag.
//
// Parameters come from the server: checkAvgCount and checkEveryMs.
BatteryReading batteryWindow(int count, uint32_t gapMs);

// Last value without a new reading — for the web page and logs.
BatteryReading batteryLast();

// Window of readings for the PUMP cell: voltage divider on GPIO35, its bottom on GPIO33.
// Enables the voltage divider itself (GPIO33 -> LOW), waits BATTERY_SETTLE_MS, takes
// count readings with a gapMs pause and turns the voltage divider off right away — unlike
// the main one, which stays on until sleep. No smoothing: one reading
// per cycle, and the percent is computed directly from the curve.
//
// The pump cell is unloaded during the reading (switch off), so the window
// here only suppresses ADC noise; the parameters still come from the server
// (row battery type="pump") so they can be controlled from the dashboard.
BatteryReading pumpBatteryWindow(int count, uint32_t gapMs);

// Timer-based polling for inactive mode. Same interval as the
// soil sensor (g_soil.soilEveryMs) — by request: the battery is checked
// as often as moisture.
void pollBattery();
