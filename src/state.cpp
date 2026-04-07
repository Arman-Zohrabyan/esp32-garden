#include "state.h"
#include <Preferences.h>

// The only place where the global state is defined. If these variables are
// declared anywhere else, linking will fail with multiple definition.
DeviceConfig  g_dev;
SoilConfig    g_soil;
BatteryConfig g_bat;
PumpConfig    g_pump;

// The RTC_DATA_ATTR attribute must be present both here and in state.h. Without it
// the variable ends up in .bss and gets zeroed on wake-up from deep sleep.
RTC_DATA_ATTR uint32_t rtcMagic      = 0;
RTC_DATA_ATTR uint32_t rtcBootCount  = 0;
RTC_DATA_ATTR uint32_t rtcFailStreak   = 0;
RTC_DATA_ATTR uint32_t rtcWearReported = 0;
RTC_DATA_ATTR uint32_t rtcWritesReported = 0;
RTC_DATA_ATTR uint32_t rtcLowBattery     = 0;
RTC_DATA_ATTR uint32_t rtcLowBatSleeps   = 0;
RTC_DATA_ATTR uint32_t rtcClockSec       = 0;
RTC_DATA_ATTR WateringRecord rtcWaterQueue[WATERING_QUEUE_SIZE];
RTC_DATA_ATTR uint32_t rtcWaterQueueLen  = 0;
RTC_DATA_ATTR uint32_t rtcWaterTimes[WATERING_RING_SIZE];
RTC_DATA_ATTR uint32_t rtcWaterTimesPos  = 0;
RTC_DATA_ATTR uint32_t rtcPostWaterBoot  = 0;
RTC_DATA_ATTR uint32_t rtcPostWaterNonce = 0;
RTC_DATA_ATTR uint32_t rtcAfterNonce     = 0;
RTC_DATA_ATTR int32_t  rtcAfterMoisture  = -1;
RTC_DATA_ATTR uint32_t rtcOfflineSkips   = 0;
RTC_DATA_ATTR char     rtcOfflineSkipReason[24];

uint32_t clockNow() {
  return rtcClockSec + millis() / 1000;
}

// Preferences is DELIBERATELY not file-static but local to each function.
// With a shared object, begin() on an already open namespace returns
// false, and all get*/put* silently keep working with the PREVIOUS namespace —
// one nested begin/end pair, and the device config would be read from soil.
// A local object costs the same bytes (a handle and two flags), and the destructor
// calls end() by itself on any exit path.

void rtcInitIfNeeded() {
  if (rtcMagic == RTC_MAGIC_VALUE) return;   // block is valid, we woke up from sleep

  // We get here on power-on, EN reset or reflashing — RTC memory
  // contains garbage, not our values.
  rtcMagic          = RTC_MAGIC_VALUE;
  rtcBootCount      = 0;
  rtcFailStreak     = 0;
  rtcWearReported   = 0;
  rtcWritesReported = 0;
  rtcLowBattery     = 0;
  rtcLowBatSleeps   = 0;
  rtcClockSec       = 0;
  rtcWaterQueueLen  = 0;
  memset(rtcWaterTimes, 0, sizeof(rtcWaterTimes));
  rtcWaterTimesPos  = 0;
  rtcPostWaterBoot  = 0;
  rtcPostWaterNonce = 0;
  rtcAfterNonce     = 0;
  rtcAfterMoisture  = -1;
  rtcOfflineSkips   = 0;
  rtcOfflineSkipReason[0] = '\0';
}

// ---------- Device ----------

void loadDeviceConfig() {
  Preferences prefs;
  prefs.begin("dev", true);                  // true = read-only
  g_dev.id              = prefs.getString("id", "");
  g_dev.isActive        = prefs.getBool("active", false);   // fail-safe
  g_dev.revision        = prefs.getInt("rev", 0);
  g_dev.appliedRevision = prefs.getInt("appliedRev", -1);
  g_dev.sleepSeconds    = prefs.getUInt("sleepSec", DEF_SLEEP_SECONDS);
  g_dev.deviceLogsSeconds = prefs.getUInt("logSec", DEF_DEVICE_LOGS_SECONDS);
  prefs.end();
}

int saveDeviceConfig() {
  Preferences prefs;
  prefs.begin("dev", false);
  int written = 0;
  // Compare before writing: the NVS partition is small and flash endurance is finite.
  // Preferences itself also skips the write on a match, but an explicit check
  // makes the intent visible and doesn't depend on library behavior.
  //
  // The defaults in get* MUST match the defaults of load*Config(): otherwise
  // a value that happens to equal the comparison default wouldn't be written when
  // the key is missing — and after a reboot load would return ITS OWN
  // default, and the config would silently roll back.
  if (prefs.getString("id", "") != g_dev.id) {
    prefs.putString("id", g_dev.id);                             written++;
  }
  if (prefs.getBool("active", false) != g_dev.isActive) {
    prefs.putBool("active", g_dev.isActive);                     written++;
  }
  if (prefs.getInt("rev", 0) != g_dev.revision) {
    prefs.putInt("rev", g_dev.revision);                         written++;
  }
  if (prefs.getInt("appliedRev", -1) != g_dev.appliedRevision) {
    prefs.putInt("appliedRev", g_dev.appliedRevision);           written++;
  }
  if (prefs.getUInt("sleepSec", DEF_SLEEP_SECONDS) != g_dev.sleepSeconds) {
    prefs.putUInt("sleepSec", g_dev.sleepSeconds);               written++;
  }
  if (prefs.getUInt("logSec", DEF_DEVICE_LOGS_SECONDS) != g_dev.deviceLogsSeconds) {
    prefs.putUInt("logSec", g_dev.deviceLogsSeconds);            written++;
  }
  prefs.end();
  return written;
}

int saveIsActive(bool active) {
  g_dev.isActive = active;
  Preferences prefs;
  // A begin() failure (corrupt NVS) must not be silent: the emergency stop
  // via the BOOT button goes through this path, and an unwritten flag would mean the board
  // keeps waking up active, as if the button had never been pressed.
  if (!prefs.begin("dev", false)) {
    Serial.println("ОШИБКА: NVS не открылся — isActive НЕ сохранён!");
    return 0;
  }
  // Compare before writing, as in save*Config(): without it a repeated
  // call with the same value would erase the sector for nothing. Right now all calls
  // are event-driven (button, click, series of failures), but if the function is ever
  // called from a loop, an unconditional write would burn through flash endurance.
  int written = 0;
  if (prefs.getBool("active", false) != active) {
    prefs.putBool("active", active);
    written = 1;
  }
  prefs.end();
  return written;
}

// ---------- Soil ----------

void loadSoilConfig() {
  Preferences prefs;
  prefs.begin("soil", true);
  g_soil.soilDry         = prefs.getInt("dry", DEF_SOIL_DRY);
  g_soil.soilWet         = prefs.getInt("wet", DEF_SOIL_WET);
  g_soil.soilEveryMs     = prefs.getUInt("everyMs", DEF_SOIL_EVERY_MS);
  g_soil.soilAvgCount    = prefs.getInt("avgCnt", DEF_SOIL_AVG_COUNT);
  g_soil.dryAlertPercent = prefs.getInt("dryPct", DEF_DRY_ALERT_PERCENT);
  g_soil.autoWaterPercent = prefs.getInt("autoPct", DEF_AUTO_WATER_PERCENT);
  g_soil.revision        = prefs.getInt("rev", 0);
  g_soil.appliedRevision = prefs.getInt("appliedRev", -1);
