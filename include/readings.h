#pragma once

#include <Arduino.h>
#include "api.h"

// Ring buffer of readings that survives loss of connectivity.
//
// Why it's needed: with sleepSeconds=3600 the board wakes up once an hour. If
// the internet drops overnight, without the buffer all readings until it returns are lost —
// and those are exactly the data the device sits in the pot for.
//
// Why NVS and not RTC memory. RTC survives deep sleep but is lost on
// brownout and an EN press — i.e. exactly in the scenarios where connectivity is already
// shaky (supply sag on battery). With an hourly cycle the buffer holds
// a day's worth of data, and losing it to a single sag is too costly.
//
// Flash wear: a normal cycle does NOT write the buffer at all — a fresh reading goes
// to the server straight from RAM, and only unsent data ends up in flash (push
// on a connectivity failure, drop on every chunk when draining the backlog).
// Endurance consumption (~100,000 cycles) is proportional to the number of FAILURES, not
// wake-ups: a board with stable Wi-Fi doesn't consume it at all, even with
// a debug sleepSeconds=50. The price of this saving — a brownout exactly at the
// moment of sending loses the one current reading; the backlog is unaffected.

// Buffer capacity. 30 records at an hourly cycle — 30 hours of autonomy,
// enough to survive an overnight router failure and most real-world
// outages. More makes little sense: day-old data is only
// interesting as history anyway, not as live readings.
#define READINGS_CAPACITY 30

// How many readings go out in one request. Sending everything at once won't do: a body of
// 30 records is about 4 KB, and with a weak signal such a POST is more likely to drop
// on timeout than get through. In chunks of 10 the upload takes three requests,
// each acknowledged separately.
#define READINGS_SEND_CHUNK 10

// A chunk must fit into the batch entirely, otherwise readingsFillBatch() would hand over
// fewer than it reports, and the difference would be silently lost when dropping from the buffer.
static_assert(READINGS_SEND_CHUNK <= REPORT_MAX_READINGS,
              "READINGS_SEND_CHUNK не должен превышать REPORT_MAX_READINGS");

// One record in compact form. int16_t where the value is guaranteed
// to fit: the ESP32 ADC is 12-bit (0..4095), percent is 0..100, RSSI in dBm is
// negative and less than 128 in magnitude. Half the size of int32_t —
// with 30 records that's the difference between 720 and 400 bytes in NVS.
struct StoredReading {
  int16_t  raw;
  int16_t  percent;      // -1 = not computed
  int16_t  minRaw;
  int16_t  maxRaw;
  int16_t  rssi;         // 0 = no data
  uint32_t bootCount;    // the wake-up during which the reading was TAKEN
  uint32_t uptimeSec;
};

// Loads the buffer from NVS. Call once in setup(), before the active cycle.
void readingsBegin();

// Puts a reading into the buffer. Called only for UNSENT ones: a reading
// that the server acknowledged in this same cycle never reaches the buffer.
// On overflow it evicts the oldest: fresh data is more valuable, and there's room for
// exactly READINGS_CAPACITY.
void readingsPush(const SoilReadingPayload& r);

// How many readings are in the buffer right now.
int readingsCount();

bool readingsEmpty();
bool readingsFull();

// Fills batch with the next chunk (up to READINGS_SEND_CHUNK items), starting
// from the oldest. Does NOT remove from the buffer — removal happens only after the
// server acknowledged receipt (see readingsDropSent).
//
// Returns how many readings were put into batch.
int readingsFillBatch(ReportBatch& batch);

// Removes the n oldest records — call after an acknowledged send.
// Specifically after acknowledgement, not after HTTP 200: the batch may have been accepted
// partially, and then data would vanish silently.
void readingsDropSent(int n);

// Clears the whole buffer. Needed when the data is so stale that
// sending it is pointless.
void readingsClear();

// ---------- flash wear tracking ----------
//
// How many times the buffer has been saved to flash over the board's lifetime. Grows on
// every putBytes, i.e. on every addition and removal of a reading.
//
// The counter itself causes no wear: it lives inside the same block that is
// written anyway, and rides along. No separate write is made for it —
// otherwise the counter would double what it measures.
//
// Why it's needed: NVS endurance is ~100,000 erase cycles per sector. A normal
// cycle doesn't write to flash (the reading goes straight from RAM), so the counter
// grows only during connectivity failures. The number in the logs shows
// the real consumption instead of guessing.
uint32_t readingsWriteCount();

// Estimate of consumed endurance in percent (0..100+).
//
// An estimate indeed: NVS spreads writes across the sector itself (wear leveling),
// so real wear is lower than the write count divided by endurance.
// Read the value as an "upper bound", not as an exact percentage.
int readingsWearPercent();
