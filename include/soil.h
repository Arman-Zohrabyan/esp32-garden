#pragma once

#include <Arduino.h>
#include "state.h"

// Soil moisture sensor: reading, averaging, open-circuit detection.
//
// Two operating modes, two separate functions — deliberately not merged into one:
//   pollSoil()      — non-blocking, for inactive mode. Called from
//                     loop(), accumulates the window across calls on a timer.
//   collectWindow() — synchronous, for the active cycle with sleep. Collects
//                     the whole window in one call and returns the result.
// Merging them would mean either blocking loop() for tens of seconds, or
// needlessly complicating the active cycle with a state machine.

// Last reading. The web page and the log read these, so we keep them global.
extern int soilRaw;       // raw ADC value, -1 = not read yet
extern int soilPercent;   // same in percent, -1 = no data

// Averaging window result.
struct SoilWindow {
  int  raw     = -1;      // mean raw over the window
  int  percent = -1;      // percent computed from the mean
  int  minRaw  = -1;      // spread within the window — shows the noise
  int  maxRaw  = -1;
  float voltage = 0.0f;
  bool sensorOk = true;   // false = looks like an open or short circuit
};

// Pin setup. Call once in setup() before the first read.
// Sensor power is OFF afterwards — turn it on explicitly for the duration of readings.
void soilBegin();

// Sensor power via PIN_SOIL_POWER. The sensor is powered not from 3V3 but from a GPIO,
// so it doesn't corrode under constant voltage.
//
// soilPowerOn() turns power on and waits SOIL_WARMUP_MS — without the pause the first
// sample will be low, as the sensor won't have time to settle.
void soilPowerOn();
void soilPowerOff();

// One measurement: averages several consecutive samples (suppresses ADC noise),
// stores the result in soilRaw / soilPercent.
//
// IMPORTANT: does not turn power on by itself — expects it to already be applied.
// This is so a window of N readings doesn't toggle power N times: the sensor
// is turned on once for the whole window (see collectWindow / pollSoil).
// For a single reading there's readSoilOnce().
void readSoil();

// Single reading with its own power cycle: turn on -> warm-up -> reading ->
// turn off. For one-off reads such as the first value at startup.
void readSoilOnce();

// Verbal assessment for the web page and the log.
const char* soilWord(int pct);

// Percent from a raw value using the current calibration g_soil.
// Returns -1 if the calibration is invalid (dry <= wet).
int soilRawToPercent(int raw);

// Signs of a disconnected sensor. The main criterion is spread: on an open circuit
// the ADC input is high-impedance and the value "floats". The secondary one is going
// outside the range (input pinned to a rail, with small spread).
bool soilLooksConnected(int avgRaw, int minRaw, int maxRaw);

// Full sensor reset: remove power, wait SOIL_RESET_MS, apply it
// again with warm-up. Needed when readings freeze or go into an
// impossible range — an unattended sensor can get stuck, and a
// restart gives a chance to bring it back without human intervention.
void soilPowerCycle();

// ACTIVE MODE: collect the whole window in one call.
// Takes count readings with a gapMs pause between them — exactly count-1 pauses,
// because a pause after the last reading would only delay sending.
// Blocks execution for ~(count-1)*gapMs, pauses via delay() (feeds the
// watchdog). Writes progress to Serial.
SoilWindow collectWindow(int count, uint32_t gapMs);

// INACTIVE MODE: timer-based polling from loop(), logs once every soilAvgCount
// readings. Blocks nothing.
void pollSoil();
