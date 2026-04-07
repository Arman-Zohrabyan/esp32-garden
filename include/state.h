#pragma once

#include <Arduino.h>
#include "config.h"

// Device state — mirrors of the server entities Device and SoilSettings.
// Grouped into structs rather than scattered across global variables: that makes them
// easier to pass to the API and log as a whole, and shows which field comes from where.

// Mirror of the server Device. Everything except id/chipId comes from the server and
// is cached in NVS namespace "dev".
struct DeviceConfig {
  String   id;                 // UUID from the server; empty string = not registered
  String   chipId;             // MAC without separators, e.g. "8C94DF8B7EF8"
  uint32_t sleepSeconds        = DEF_SLEEP_SECONDS;
  uint32_t deviceLogsSeconds   = DEF_DEVICE_LOGS_SECONDS;
  bool     isActive            = false;   // fail-safe: inactive by default
  int32_t  revision            = 0;
  int32_t  appliedRevision     = -1;      // -1 = nothing confirmed yet
};

// Mirror of the server SoilSettings, cached in NVS namespace "soil".
struct SoilConfig {
  int32_t  soilDry         = DEF_SOIL_DRY;
  int32_t  soilWet         = DEF_SOIL_WET;
  uint32_t soilEveryMs     = DEF_SOIL_EVERY_MS;
  int32_t  soilAvgCount    = DEF_SOIL_AVG_COUNT;
  int32_t  dryAlertPercent = DEF_DRY_ALERT_PERCENT;
  int32_t  autoWaterPercent = DEF_AUTO_WATER_PERCENT;   // auto-watering threshold
  int32_t  revision        = 0;
  int32_t  appliedRevision = -1;
};

// Mirror of the server battery row with type="esp32", cached in NVS namespace "bat".
// The type="pump" row lives in PumpConfig (batCheck*): these are settings of the
// same pump unit, and there's no reason to cache them separately.
//
// battery has no revision of its own on the server, so there's nothing to confirm:
// values are applied as received.
struct BatteryConfig {
  uint32_t checkEveryMs  = DEF_BAT_EVERY_MS;
  int32_t  checkAvgCount = DEF_BAT_AVG_COUNT;
  int32_t  alertPercent  = DEF_BAT_ALERT_PERCENT;
};

// Mirror of the server PumpSettings (cached in NVS namespace "pump") plus the pump
// cell reading window from the battery row type="pump". The cache is needed for watering
// without connectivity: then the board waters using the last received settings.
struct PumpConfig {
  bool     autoWaterEnabled      = DEF_PUMP_ENABLED;
  int32_t  waterMl               = DEF_WATER_ML;
  float    flowMlPerSec          = DEF_PUMP_FLOW;
  uint32_t postWaterSleepSeconds = DEF_POST_WATER_SLEEP;
  int32_t  maxWateringsPerDay    = DEF_MAX_WATERINGS_DAY;
  float    minBatteryVolt        = DEF_PUMP_MIN_VOLT;
  int32_t  maxPumpSeconds        = DEF_MAX_PUMP_SECONDS;
  int32_t  revision              = 0;
  int32_t  appliedRevision       = -1;
  uint32_t batCheckEveryMs       = DEF_BAT_EVERY_MS;   // battery type="pump"
  int32_t  batCheckAvgCount      = DEF_BAT_AVG_COUNT;
};

extern DeviceConfig  g_dev;
extern SoilConfig    g_soil;
extern BatteryConfig g_bat;
extern PumpConfig    g_pump;

// ---------- RTC memory ----------
//
// RTC_DATA_ATTR survives deep sleep but is lost on a full reset
// (power-on, EN button, reflashing). The attribute MUST be present both here, on
// the extern declaration, and on the definition in state.cpp — otherwise the variable
// ends up in regular .bss and is silently zeroed on wake-up.

// Value used to check the RTC block's validity after a reset.
#define RTC_MAGIC_VALUE 0xE5327BADu

extern RTC_DATA_ATTR uint32_t rtcMagic;
extern RTC_DATA_ATTR uint32_t rtcBootCount;    // incremented on every wake-up
extern RTC_DATA_ATTR uint32_t rtcFailStreak;   // consecutive cycles without connectivity

// The last NVS wear percentage already reported to the server. Needed
// so the warning is sent once per threshold crossing, not
// every cycle. It lives in RTC, not NVS, on purpose: storing it in flash
// would mean wearing out flash to track flash wear.
//
// Losing it on power-on is harmless: at worst the warning arrives
// again, which is less harmful than missing it.
extern RTC_DATA_ATTR uint32_t rtcWearReported;

// The nvsWrites value up to which (inclusive) usage has already been reported to the server
// via the nvs_write log. Updated ONLY after a confirmed send: otherwise a
// log lost along with a failed report would never be repeated.
// Lives in RTC for the same reason as rtcWearReported: storing it in NVS
// would mean wearing out flash to track flash wear. Losing it on power-on
// produces one log with an "all-time" delta — less harmful than missing it.
extern RTC_DATA_ATTR uint32_t rtcWritesReported;

// Cell protection: 1 = the cell dropped below BATTERY_LOW_VOLTS and has not since
// risen above BATTERY_RESUME_VOLTS. While the flag is set, the active cycle doesn't
// turn on Wi-Fi and sleeps for 12 hours at a time. In RTC, not NVS: after power-on
// the very first low reading will set it again.
extern RTC_DATA_ATTR uint32_t rtcLowBattery;

// How many times in a row the board went to sleep due to a depleted cell without connecting.
// Sent to the server as a battery_low_sleep log with the first report once the cell
// is charged — otherwise a multi-hour gap in the data would remain unexplained.
extern RTC_DATA_ATTR uint32_t rtcLowBatSleeps;

// Board clock: seconds since the last power-on, including sleep time. goToSleep()
// adds the awake time and the sleep length to it, clockNow() returns the current
// value. Our own, not time(): it needs neither NTP nor the server, and it runs
// steadily through deep sleep. Accuracy is that of the sleep timer (RC oscillator,
// a few percent), which is enough for the daily watering limit.
extern RTC_DATA_ATTR uint32_t rtcClockSec;
uint32_t clockNow();

// One watering waiting to be sent to the server. nonce is a random id: with it
// the server doesn't create a duplicate if the report response was lost and the watering was sent
// again, and it also uses it to find the watering for moistureAfter.
struct WateringRecord {
  uint32_t nonce;
  uint32_t bootCount;
  int32_t  ml;             // requested dose
  uint32_t pumpMs;         // how long the pump actually ran
  float    flowMlPerSec;
  int32_t  moistureBefore;
  int32_t  moistureAfter;  // -1 = not measured yet
  float    pumpVolt;       // pump cell before watering
  uint8_t  offline;        // 1 = watered from cache, without a server response
  uint32_t at;             // clockNow() at the moment of watering
};

// Queue of waterings to send. In RTC, not NVS: there are only a few waterings a day,
// and losing a record on power loss is cheaper than writing flash.
extern RTC_DATA_ATTR WateringRecord rtcWaterQueue[WATERING_QUEUE_SIZE];
extern RTC_DATA_ATTR uint32_t       rtcWaterQueueLen;

// clockNow() of recent waterings — for the daily limit without connectivity. 0 = empty.
extern RTC_DATA_ATTR uint32_t rtcWaterTimes[WATERING_RING_SIZE];
extern RTC_DATA_ATTR uint32_t rtcWaterTimesPos;

// The watering for which a check reading is due: rtcBootCount of that
// wake-up and the watering's nonce. Only the NEXT wake-up counts as the check
// (the sleep after watering); otherwise the flag is stale and gets cleared.
extern RTC_DATA_ATTR uint32_t rtcPostWaterBoot;
extern RTC_DATA_ATTR uint32_t rtcPostWaterNonce;

// Check reading for a watering that has already been sent to the server — it goes out
// as a separate wateringsAfter. nonce 0 = nothing to wait for.
extern RTC_DATA_ATTR uint32_t rtcAfterNonce;
extern RTC_DATA_ATTR int32_t  rtcAfterMoisture;

// Skipped waterings without connectivity (dry, but watering not allowed): the count and the last
// reason. Sent as a single watering_skipped log when connectivity returns.
extern RTC_DATA_ATTR uint32_t rtcOfflineSkips;
extern RTC_DATA_ATTR char     rtcOfflineSkipReason[24];

// Unsent readings are NOT stored here but in NVS — see readings.h.
// RTC memory wouldn't preserve them: it is lost on brownout and power
// loss, i.e. exactly in the cases where connectivity is already shaky. With an
// hourly cycle the buffer holds a day's worth of data, and losing it is too costly.

// Resets RTC variables to their initial state if the block is invalid
// (first boot or power reset). Call at the very start of setup().
void rtcInitIfNeeded();

// ---------- NVS ----------
//
// NVS keys are limited to 15 characters — hence abbreviations like "sleepSec".
// We write only on an actual change: the NVS partition is 20 KB, flash endurance is
// ~100k cycles, and with sleepSeconds=50 that's ~1700 wake-ups per day.

// save* return the number of keys ACTUALLY written (0 = everything matched, flash
// untouched). Each written key is a spent NVS cycle, and using
// this number the caller reports usage to the server via the nvs_write log.
void loadDeviceConfig();   // NVS "dev" -> g_dev (missing = defaults)
int  saveDeviceConfig();   // g_dev -> NVS "dev"
void loadSoilConfig();     // NVS "soil" -> g_soil
int  saveSoilConfig();     // g_soil -> NVS "soil"
void loadBatteryConfig();  // NVS "bat"  -> g_bat
int  saveBatteryConfig();  // g_bat -> NVS "bat"
void loadPumpConfig();     // NVS "pump" -> g_pump
int  savePumpConfig();     // g_pump -> NVS "pump"

// Saves only the active flag — used in the emergency stop via the
// BOOT button, where it's important to write it before any network operations.
// Returns the number of keys written (0 or 1), like save*Config().
int saveIsActive(bool active);

// Migration from the old scheme: namespace "collect" replaced by "dev"/"active".
// If the old flag was set, treat it as INactive (fail-safe) —
// otherwise after the update the board would go to sleep in a state unknown to the server.
void migrateLegacyCollectFlag();
