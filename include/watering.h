#pragma once

#include <Arduino.h>
#include "api.h"

// Auto-watering (firmware 0.4.0).
//
// Cycle: soil reading -> moisture not above g_soil.autoWaterPercent -> checks
// (pump cell, daily limit) -> ask the server "allowed, and how much?" ->
// pump for ml / (ml/s) seconds -> sleep g_pump.postWaterSleepSeconds ->
// follow-up reading (moistureAfter) -> regular sleep g_dev.sleepSeconds.
//
// Without a connection it's the same, but the dose and limits come from the NVS cache, and the watering
// waits in the RTC queue to be sent. Offline skips accumulate in a counter and
// go out as a single log when the connection returns.

// Decision and watering for the current wake-up. Call after the soil reading.
//   percent — this cycle's moisture, -1 = the sensor gave no reading;
//   pumpBat — pump cell reading, percent < 0 = unreliable;
//   online  — the server responded this cycle: then before watering the board
//             asks it, otherwise it waters from the cache;
//   batch   — where to put logs; nullptr = no connection at all.
// true = the pump ran, sleep for g_pump.postWaterSleepSeconds.
bool autoWaterStep(int percent, const BatteryLogPayload& pumpBat,
                   bool online, ReportBatch* batch);

// What from RTC went into the batch — so exactly that is removed after delivery.
struct WateringSent {
  int  queued = 0;      // waterings from the queue
  bool after  = false;  // follow-up reading for an already-sent watering
  bool skips  = false;  // summary of offline skips
};

// Puts everything awaiting sending into the batch: the watering queue, the follow-up
// reading, the skips summary. Call AFTER this cycle's autoWaterStep.
WateringSent wateringAddPending(ReportBatch& batch);

// A batch with this content was accepted by the server — remove what was sent from RTC.
void wateringMarkDelivered(const WateringSent& sent);
