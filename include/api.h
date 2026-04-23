#pragma once

#include <Arduino.h>
#include "state.h"

// Communication with the esp32.novacoteam-server.com backend over HTTP + JSON.
//
// All communication boils down to TWO requests:
//   sync   — at the start of the cycle: registration + config + revision acks
//            + battery readings upload, all in one call;
//   report — at the end of the cycle: soil readings, logs and waterings, all arrays
//            optional.
// A third one — watering — only in a cycle where it's time to water: right before
// turning the pump on, the board asks whether it may water and how much.
//
// The old register/ping/confirm/soil-settings/soil-readings/device-logs are
// gone from the server — this is a breaking change, there's no rolling back.

// Why not a plain bool: it loses the distinction between "Wi-Fi not up",
// "server returned 404" and "got malformed JSON". And that's exactly what we
// need to branch on: transport error — "try later", ParseError — "log it,
// something's wrong". A 404 became impossible after moving to sync: the server
// creates unknown chipIds itself, so there's no re-registration branch anymore.
enum class ApiStatus : uint8_t {
  Ok = 0,
  NoWifi,       // Wi-Fi not connected — the request wasn't even sent
  Transport,    // DNS/TCP/timeout, httpCode < 0
  HttpError,    // server responded 4xx/5xx
  ParseError,   // 200 response, but the body couldn't be parsed
};

struct ApiResult {
  ApiStatus status   = ApiStatus::NoWifi;
  int       httpCode = 0;
  bool ok() const { return status == ApiStatus::Ok; }
  const char* cstr() const;      // for logs: "ok" / "no_wifi" / ...
};

// One averaged soil reading. minRaw/maxRaw — the spread within the window; from
// it the server can see noise and sensor contact quality.
struct SoilReadingPayload {
  int32_t  raw       = 0;
  int32_t  percent   = -1;    // -1 = not computed, field goes out as null
  float    voltage   = 0.0f;
  int32_t  minRaw    = 0;
  int32_t  maxRaw    = 0;
  int32_t  rssi      = 0;     // 0 = no data, field goes out as null
  uint32_t uptimeSec = 0;
  uint32_t bootCount = 0;     // 0 = don't send; the counter is always >= 1
};

// ---------- sync ----------

// What the firmware learns from the sync response beyond what went into g_dev/g_soil.
struct SyncOutcome {
  bool devChanged  = false;   // device revision differs from the applied one
  bool soilChanged = false;   // same for soil settings
  bool pumpChanged = false;   // same for pump settings
  int32_t prevDevRev  = 0;    // for the config_applied log
  int32_t prevSoilRev = 0;
  int32_t prevPumpRev = 0;
  int     batteryAccepted = 0;
  int     batteryRejected = 0;

  // Battery settings arrived in the response. They have no revision of their
  // own on the server, so there's nothing to compare against — we just note that
  // they were received, so the caller knows the cache should be updated.
  bool batteryCfgChanged = false;

  // The pump block (pump settings) or a battery row with type="pump" arrived in
  // the response — the pump NVS cache should be checked (savePumpConfig compares itself).
  bool pumpReceived = false;
};

// Battery reading to send: "esp32" — the board cell, "pump" — the pump cell.
struct BatteryLogPayload {
  const char* type    = "esp32";
  float       volt    = 0.0f;
  int32_t     percent = -1;   // -1 = don't send the record at all
};

// POST /devices/by-chip/:chipId/sync — the only call at the start of the cycle.
//
// Replaces register + ping + GET soil-settings + both confirms at once.
// The server registers an unknown chipId itself, so the firmware no longer has
// a separate "we were deleted, re-register" branch: a database wipe
// and the first power-on go down the very same path.
//
// percent < 0 on bat / pumpBat -> that cell wasn't measured this cycle (or the reading
// is unreliable), its record isn't sent; if both are like that, the array isn't sent at all.
// Fills dev, soil, batCfg and pump; returns the revision differences via out.
ApiResult apiSync(DeviceConfig& dev, SoilConfig& soil, BatteryConfig& batCfg,
                  PumpConfig& pump, const BatteryLogPayload& bat,
                  const BatteryLogPayload& pumpBat, SyncOutcome& out);

// ---------- watering ----------

// Server reply to "may I water?". reason is empty when allowed; otherwise
// disabled | daily_limit | pump_battery_low | not_dry.
struct WateringReply {
  bool    allowed        = false;
  char    reason[24]     = "";
  int32_t ml             = 0;
  float   flowMlPerSec   = 0.0f;
  int32_t maxPumpSeconds = 0;
  int32_t wateringsToday = 0;
};

// POST /devices/by-chip/:chipId/watering — right before turning the pump on.
// The server replies with the dose (ml) and flow rate per the VERY latest
// dashboard settings; the board computes the pump run time itself.
ApiResult apiRequestWatering(const DeviceConfig& dev, int moisture,
                             float pumpVolt, WateringReply& out);

// PUT /devices/:id — the firmware uses it ONLY for isActive.
//
// The ownership rule is asymmetric: false->true goes only through this path
// (the local garden.local page), because sync on an already known
// device doesn't override the server value. true->false can be done by both:
// the server and the board (BOOT button, reported on the next sync).
//
// Requires dev.id — the UUID comes in the sync response and is stored in NVS.
ApiResult apiPutIsActive(const DeviceConfig& dev, bool isActive);

// ---------- report ----------

// Per-cycle log accumulator. A single wake-up can produce several logs
// (boot after brownout + wifi_failed + cycle_done), and previously each went out
// as a separate request. Now we accumulate them and send in one batch.
//
// Capacity 14 with headroom: the worst real case is boot + wifi_failed +
// battery_low_sleep + three config_applied + nvs_write(config) +
// sensor_disconnected + watering(_skipped) + watering_skipped (skips without
// connectivity) + cycle_done + nvs_wear + nvs_write(buffer) = 13. Overflow doesn't
// crash the board, it drops the excess with a note to the serial port.
#define REPORT_MAX_LOGS 14

// How many readings fit into one report. Matches READINGS_SEND_CHUNK
// from readings.h: the buffer hands out chunks of exactly this size, and the batch must
// be able to take a whole chunk. Given as a number rather than via include —
// api.h must not depend on readings.h, the dependency goes the other
// way (readings.h includes api.h for SoilReadingPayload).
#define REPORT_MAX_READINGS 10

class ReportBatch {
public:
  // level: "info" | "warn" | "error"
  // event: machine identifier ("wifi_failed", not "WiFi reconnect failed") —
  //        the server filters and groups by it.
  // extraJson: raw fragment like "\"reason\":\"PANIC\"" or nullptr.
  //        The skeleton (chipId/level/event/uptimeSec) is added on send.
  //        A raw fragment rather than a JsonDocument — saves RAM; all calls
  //        pass literals from the code, there are no user strings there.
  // bootCount: 0 = take the current one; another value is needed when the log describes
  //        a DIFFERENT wake-up (brownout is logged from the next cycle).
  void addLog(const char* level, const char* event,
              const char* extraJson = nullptr, uint32_t bootCount = 0);

  void addReading(const SoilReadingPayload& r);

  // A watering from the RTC queue and a follow-up reading for an already sent watering.
  void addWatering(const WateringRecord& w);
  void addWateringAfter(uint32_t nonce, int32_t moistureAfter);

  bool empty() const {
    return logCount == 0 && readingCount == 0 && wateringCount == 0 && afterCount == 0;
  }

  int logs()     const { return logCount; }
  int readings() const { return readingCount; }

  friend ApiResult apiReport(DeviceConfig&, ReportBatch&, class ReportOutcome&);

private:
  struct LogEntry {
    String   level;
    String   event;
    String   extra;
    uint32_t bootCount;
  };

  LogEntry           logEntries[REPORT_MAX_LOGS];
  int                logCount = 0;
  SoilReadingPayload readingEntries[REPORT_MAX_READINGS];
  int                readingCount = 0;

  WateringRecord     wateringEntries[WATERING_QUEUE_SIZE];
  int                wateringCount = 0;
  uint32_t           afterNonce    = 0;   // one follow-up reading per cycle
  int32_t            afterMoisture = -1;
  int                afterCount    = 0;
};

// What the server returned for report.
class ReportOutcome {
public:
  int readingsAccepted = 0;
  int readingsRejected = 0;
  int logsAccepted     = 0;
  int logsRejected     = 0;
  int wateringsAccepted = 0;
  int wateringsRejected = 0;

  // Current server state, echoed in the response. Needed to catch a
  // deactivation that happened WHILE the reading cycle was running (~20 s), and not sleep for
  // sleepSeconds in a state the server has already cancelled.
  bool    isActive       = false;
  bool    haveDeviceInfo = false;   // false = server didn't send the device block
  int32_t revision       = 0;

  uint32_t serverTime = 0;          // Unix seconds, 0 = not sent

  // The buffer may be cleared only when the server confirmed it accepted everything.
  // An HTTP 200 alone isn't enough: the batch may have been accepted partially.
  bool readingsFullyAccepted() const { return readingsRejected == 0; }
};

// POST /devices/by-chip/:chipId/report — readings and logs in one request.
//
// Both arrays are independently optional; the server accepts an empty body as a
// no-op. Returns ApiStatus::Ok even when some records were rejected —
// check the counters in out, not just the HTTP code.
ApiResult apiReport(DeviceConfig& dev, ReportBatch& batch, ReportOutcome& out);
