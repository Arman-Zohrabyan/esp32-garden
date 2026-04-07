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
  prefs.end();
}

int saveSoilConfig() {
  Preferences prefs;
  prefs.begin("soil", false);
  int written = 0;
  // The get* defaults mirror loadSoilConfig() — see the comment in
  // saveDeviceConfig().
  if (prefs.getInt("dry", DEF_SOIL_DRY) != g_soil.soilDry) {
    prefs.putInt("dry", g_soil.soilDry);                         written++;
  }
  if (prefs.getInt("wet", DEF_SOIL_WET) != g_soil.soilWet) {
    prefs.putInt("wet", g_soil.soilWet);                         written++;
  }
  if (prefs.getUInt("everyMs", DEF_SOIL_EVERY_MS) != g_soil.soilEveryMs) {
    prefs.putUInt("everyMs", g_soil.soilEveryMs);                written++;
  }
  if (prefs.getInt("avgCnt", DEF_SOIL_AVG_COUNT) != g_soil.soilAvgCount) {
    prefs.putInt("avgCnt", g_soil.soilAvgCount);                 written++;
  }
  if (prefs.getInt("dryPct", DEF_DRY_ALERT_PERCENT) != g_soil.dryAlertPercent) {
    prefs.putInt("dryPct", g_soil.dryAlertPercent);              written++;
  }
  if (prefs.getInt("autoPct", DEF_AUTO_WATER_PERCENT) != g_soil.autoWaterPercent) {
    prefs.putInt("autoPct", g_soil.autoWaterPercent);            written++;
  }
  if (prefs.getInt("rev", 0) != g_soil.revision) {
    prefs.putInt("rev", g_soil.revision);                        written++;
  }
  if (prefs.getInt("appliedRev", -1) != g_soil.appliedRevision) {
    prefs.putInt("appliedRev", g_soil.appliedRevision);          written++;
  }
  prefs.end();
  return written;
}

// ---------- Battery ----------

void loadBatteryConfig() {
  Preferences prefs;
  prefs.begin("bat", true);
  g_bat.checkEveryMs  = prefs.getUInt("everyMs", DEF_BAT_EVERY_MS);
  g_bat.checkAvgCount = prefs.getInt("avgCnt", DEF_BAT_AVG_COUNT);
  g_bat.alertPercent  = prefs.getInt("alertPct", DEF_BAT_ALERT_PERCENT);
  prefs.end();
}

int saveBatteryConfig() {
  Preferences prefs;
  prefs.begin("bat", false);
  int written = 0;
  // The get* defaults mirror loadBatteryConfig() — see the comment in
  // saveDeviceConfig().
  if (prefs.getUInt("everyMs", DEF_BAT_EVERY_MS) != g_bat.checkEveryMs) {
    prefs.putUInt("everyMs", g_bat.checkEveryMs);                written++;
  }
  if (prefs.getInt("avgCnt", DEF_BAT_AVG_COUNT) != g_bat.checkAvgCount) {
    prefs.putInt("avgCnt", g_bat.checkAvgCount);                 written++;
  }
  if (prefs.getInt("alertPct", DEF_BAT_ALERT_PERCENT) != g_bat.alertPercent) {
    prefs.putInt("alertPct", g_bat.alertPercent);                written++;
  }
  prefs.end();
  return written;
}

// ---------- Pump ----------

void loadPumpConfig() {
  Preferences prefs;
  prefs.begin("pump", true);
  g_pump.autoWaterEnabled      = prefs.getBool("on", DEF_PUMP_ENABLED);
  g_pump.waterMl               = prefs.getInt("ml", DEF_WATER_ML);
  g_pump.flowMlPerSec          = prefs.getFloat("flow", DEF_PUMP_FLOW);
  g_pump.postWaterSleepSeconds = prefs.getUInt("postSleep", DEF_POST_WATER_SLEEP);
  g_pump.maxWateringsPerDay    = prefs.getInt("perDay", DEF_MAX_WATERINGS_DAY);
  g_pump.minBatteryVolt        = prefs.getFloat("minVolt", DEF_PUMP_MIN_VOLT);
  g_pump.maxPumpSeconds        = prefs.getInt("maxSec", DEF_MAX_PUMP_SECONDS);
  g_pump.revision              = prefs.getInt("rev", 0);
  g_pump.appliedRevision       = prefs.getInt("appliedRev", -1);
  g_pump.batCheckEveryMs       = prefs.getUInt("batEvery", DEF_BAT_EVERY_MS);
  g_pump.batCheckAvgCount      = prefs.getInt("batAvg", DEF_BAT_AVG_COUNT);
  prefs.end();
}

int savePumpConfig() {
  Preferences prefs;
  prefs.begin("pump", false);
  int written = 0;
  // The get* defaults mirror loadPumpConfig() — see the comment in
  // saveDeviceConfig(). floats are compared exactly: the value is parsed every time
  // from the same JSON and yields the same float.
  if (prefs.getBool("on", DEF_PUMP_ENABLED) != g_pump.autoWaterEnabled) {
    prefs.putBool("on", g_pump.autoWaterEnabled);                written++;
  }
  if (prefs.getInt("ml", DEF_WATER_ML) != g_pump.waterMl) {
    prefs.putInt("ml", g_pump.waterMl);                          written++;
  }
  if (prefs.getFloat("flow", DEF_PUMP_FLOW) != g_pump.flowMlPerSec) {
    prefs.putFloat("flow", g_pump.flowMlPerSec);                 written++;
  }
  if (prefs.getUInt("postSleep", DEF_POST_WATER_SLEEP) != g_pump.postWaterSleepSeconds) {
    prefs.putUInt("postSleep", g_pump.postWaterSleepSeconds);    written++;
  }
  if (prefs.getInt("perDay", DEF_MAX_WATERINGS_DAY) != g_pump.maxWateringsPerDay) {
    prefs.putInt("perDay", g_pump.maxWateringsPerDay);           written++;
  }
  if (prefs.getFloat("minVolt", DEF_PUMP_MIN_VOLT) != g_pump.minBatteryVolt) {
    prefs.putFloat("minVolt", g_pump.minBatteryVolt);            written++;
  }
  if (prefs.getInt("maxSec", DEF_MAX_PUMP_SECONDS) != g_pump.maxPumpSeconds) {
    prefs.putInt("maxSec", g_pump.maxPumpSeconds);               written++;
  }
  if (prefs.getInt("rev", 0) != g_pump.revision) {
    prefs.putInt("rev", g_pump.revision);                        written++;
  }
  if (prefs.getInt("appliedRev", -1) != g_pump.appliedRevision) {
    prefs.putInt("appliedRev", g_pump.appliedRevision);          written++;
  }
  if (prefs.getUInt("batEvery", DEF_BAT_EVERY_MS) != g_pump.batCheckEveryMs) {
    prefs.putUInt("batEvery", g_pump.batCheckEveryMs);           written++;
  }
  if (prefs.getInt("batAvg", DEF_BAT_AVG_COUNT) != g_pump.batCheckAvgCount) {
    prefs.putInt("batAvg", g_pump.batCheckAvgCount);             written++;
  }
  prefs.end();
  return written;
}

// ---------- migration ----------

void migrateLegacyCollectFlag() {
  Preferences prefs;
  prefs.begin("collect", true);
  bool hadFlag = prefs.isKey("on");
  bool wasOn   = prefs.getBool("on", false);
  prefs.end();

  if (!hadFlag) return;                      // nothing to migrate

  // The old collection flag was set — the board would go to sleep right after the update,
  // and the server knows nothing about it. Fail-safe: clear the flag and go
  // to web mode, from where the user activates the device again.
  Serial.printf("Миграция: старый флаг collect/on = %s -> isActive = false\n",
                wasOn ? "true" : "false");

  prefs.begin("collect", false);
  prefs.clear();                             // namespace is no longer used
  prefs.end();

  saveIsActive(false);
}
