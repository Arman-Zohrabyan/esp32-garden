#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <esp_ota_ops.h>
#include <HTTPClient.h>
#include <esp_sleep.h>
#include "config.h"        // all constants and defaults
#include "state.h"         // g_dev / g_soil / RTC variables
#include "soil.h"          // moisture sensor
#include "net.h"           // Wi-Fi, AP, scan, mDNS garden.local
#include "web.h"           // setup portal and dashboard
#include "sleepmgr.h"      // chipId, reset reason, deep sleep
#include "api.h"           // backend communication
#include "battery.h"       // battery charge reading on GPIO34
#include "readings.h"      // readings buffer for offline periods
#include "watering.h"      // auto-watering: pump on GPIO27, pump cell on GPIO35

// main.cpp is orchestration only: setup(), loop() and transitions between modes.
// Sensor in soil.cpp, Wi-Fi in net.cpp, page in web.cpp, server in api.cpp,
// constants in config.h, state in state.cpp.
//
// Server communication is reduced to two requests per cycle: sync at the start (config,
// registration, revision acknowledgement, battery) and report at the end (readings and
// logs in one batch). Previously there were 4-6, and 9-10 in the worst case.

// Defined below; prototypes so that the order of functions in the file doesn't dictate
// the call order.
void syncWithServer();
bool syncConfig(ReportBatch* batch = nullptr);

// Acknowledges applied revisions. Call AFTER the values have
// actually been applied; the server learns about it from the next sync.
//
// A separate function because the timing matters: appliedRevision means
// "I applied", not "I received". Setting it right when parsing the response
// would mean lying to the server if the board crashed between receiving and
// applying the config.
static void markConfigApplied(const SyncOutcome& sync, ReportBatch* batch) {
  bool changed = false;

  if (sync.devChanged) {
    if (batch) {
      char extra[160];
      snprintf(extra, sizeof(extra),
               "\"revision\":%d,\"prevRevision\":%d,\"scope\":\"device\","
               "\"sleepSeconds\":%lu,\"isActive\":%s",
               g_dev.revision, sync.prevDevRev,
               (unsigned long)g_dev.sleepSeconds,
               g_dev.isActive ? "true" : "false");
      batch->addLog("info", "config_applied", extra);
    }
    g_dev.appliedRevision = g_dev.revision;
    changed = true;
  }

  if (sync.soilChanged) {
    if (batch) {
      char extra[220];
      snprintf(extra, sizeof(extra),
               "\"revision\":%d,\"prevRevision\":%d,\"scope\":\"soil\","
               "\"soilDry\":%d,\"soilWet\":%d,\"soilEveryMs\":%lu,\"soilAvgCount\":%d,"
               "\"autoWaterPercent\":%d",
               g_soil.revision, sync.prevSoilRev,
               g_soil.soilDry, g_soil.soilWet,
               (unsigned long)g_soil.soilEveryMs, g_soil.soilAvgCount,
               g_soil.autoWaterPercent);
      batch->addLog("info", "config_applied", extra);
    }
    g_soil.appliedRevision = g_soil.revision;
    changed = true;
  }

  if (sync.pumpChanged) {
    if (batch) {
      char extra[260];
      snprintf(extra, sizeof(extra),
               "\"revision\":%d,\"prevRevision\":%d,\"scope\":\"pump\","
               "\"autoWaterEnabled\":%s,\"waterMl\":%d,\"flowMlPerSec\":%.2f,"
               "\"postWaterSleepSeconds\":%lu,\"maxWateringsPerDay\":%d,"
               "\"minBatteryVolt\":%.2f,\"maxPumpSeconds\":%d",
               g_pump.revision, sync.prevPumpRev,
               g_pump.autoWaterEnabled ? "true" : "false", g_pump.waterMl,
               g_pump.flowMlPerSec, (unsigned long)g_pump.postWaterSleepSeconds,
               g_pump.maxWateringsPerDay, g_pump.minBatteryVolt,
               g_pump.maxPumpSeconds);
      batch->addLog("info", "config_applied", extra);
    }
    g_pump.appliedRevision = g_pump.revision;
  }

  // Touch NVS only when something actually changed: flash endurance is
  // ~100k cycles, and with sleepSeconds=50 there are about 1700 wake-ups per day.
  int keysWritten = 0;
  if (changed) {
    keysWritten += saveDeviceConfig();
    keysWritten += saveSoilConfig();
  }

  // Battery settings are saved separately: they have no revision of their own, and
  // saveBatteryConfig() compares the values itself before writing; if the
  // server sent the same values, nothing goes to flash.
  if (sync.batteryCfgChanged) keysWritten += saveBatteryConfig();

  // Pump: the same, on every response: the board waters offline from this cache,
  // and it must match the server, not the moment of the last revision.
  if (sync.pumpReceived) keysWritten += savePumpConfig();

  // Every written key is a consumed NVS cycle, so report it right away:
  // the config only changes on a live sync, so the log goes out with the next
  // report in this same cycle. batch == nullptr only on the deactivation path,
  // where there will be no report; a trace remains in Serial there.
  if (keysWritten > 0) {
    Serial.printf("NVS: записано ключей конфига: %d\n", keysWritten);
    if (batch) {
      char w[64];
      snprintf(w, sizeof(w), "\"scope\":\"config\",\"keysWritten\":%d",
               keysWritten);
      batch->addLog("info", "nvs_write", w);
    }
  }
}

// Collects a battery reading to send in sync. Returns percent = -1
// if the reading is unreliable (jumper J2 removed or the voltage divider didn't turn on;
// see BATTERY_VALID_MIN_VOLTS / BATTERY_VALID_MAX_VOLTS in config.h).
static BatteryLogPayload readBatteryForSync() {
  BatteryLogPayload out;

  // Reading window per the server settings: checkAvgCount readings with a pause of
  // checkEveryMs. Spread out over time on purpose; see batteryWindow().
  Serial.printf("Батарея: %d замеров с паузой %lu мс\n",
                g_bat.checkAvgCount, (unsigned long)g_bat.checkEveryMs);
  BatteryReading b = batteryWindow(g_bat.checkAvgCount, g_bat.checkEveryMs);

  // Too low a voltage means not a discharge but an unreliable reading:
  // jumper J2 removed or the voltage divider disconnected. A live cell never gets
  // that low; the TP4056 protection kicks in first. Too high means the divider
  // didn't turn on and the ADC hit its ceiling. We leave percent = -1, and
  // apiSync won't send such a record.
  if (b.voltage < BATTERY_VALID_MIN_VOLTS) {
    Serial.printf("Батарея: замер недостоверен (%.2f В) — снята J2? Не отправляю\n",
                  b.voltage);
    return out;
  }
  if (b.voltage > BATTERY_VALID_MAX_VOLTS) {
    Serial.printf("Батарея: замер недостоверен (%.2f В) — делитель не включился "
                  "(GPIO%d)? Не отправляю\n", b.voltage, PIN_BAT_EN);
    return out;
  }

  out.volt    = b.voltage;
  out.percent = b.percent;
  Serial.printf("Батарея: %d%% (%.2f В)\n", out.percent, out.volt);
  return out;
}

// Pump cell reading for sync: same idea as readBatteryForSync(),
// but the voltage divider on GPIO35 is enabled only for the window. The window comes from the
// battery type="pump" row (dashboard, "Pump battery"). percent = -1 if the reading
// is unreliable: J5 removed, cell disconnected or its protection tripped.
static BatteryLogPayload readPumpBatteryForSync() {
  BatteryLogPayload out;
  out.type = "pump";

  Serial.printf("Банка насоса: %d замеров с паузой %lu мс\n",
                g_pump.batCheckAvgCount, (unsigned long)g_pump.batCheckEveryMs);
  BatteryReading b = pumpBatteryWindow(g_pump.batCheckAvgCount, g_pump.batCheckEveryMs);

  if (b.voltage < BATTERY_VALID_MIN_VOLTS || b.voltage > BATTERY_VALID_MAX_VOLTS) {
    Serial.printf("Банка насоса: замер недостоверен (%.2f В, на пине %d мВ) — "
                  "снята J5 или банка отключена? Не отправляю\n",
                  b.voltage, b.mvOnPin);
    out.volt = b.voltage;   // for the watering-skip log; not sent to the server
    return out;
  }

  out.volt    = b.voltage;
  out.percent = b.percent;
  Serial.printf("Банка насоса: %d%% (%.2f В)\n", out.percent, out.volt);
  return out;
}

// ---------- active cycle stages ----------
//
// Each function below is one linear stage of activeCycle(). Branching and
// the "sleep / go to web mode / continue" decisions stay in
// activeCycle() itself: all exit paths are visible there at once, while here there is only
// the straightforward work of the stages.

// Emergency stop via the BOOT button. Saved locally right away, the server is told
// on the next sync; guaranteed exit from sleep takes priority. true = stopped.
static bool emergencyStopRequested() {
  if (!bootButtonPressed()) return false;

  saveIsActive(false);
  Serial.println("\n>>> BOOT нажата — активный режим ОСТАНОВЛЕН.");
  pinMode(PIN_LED, OUTPUT);
  for (int i = 0; i < 2; i++) {
    digitalWrite(PIN_LED, HIGH); delay(400);
    digitalWrite(PIN_LED, LOW);  delay(200);
  }
  return true;
}

// Brings up Wi-Fi using the saved credentials. STA only: we don't start the access point or
// the web server; they would eat RAM and radio time, and are unreachable during sleep
// anyway.
static bool wifiConnectFromNvs() {
  WiFi.mode(WIFI_STA);

  Preferences p;
  p.begin("wifi", true);
  String ssid = p.getString("ssid", "");
  String pass = p.getString("pass", "");
  p.end();

  if (!ssid.length()) return false;
  return waitConnect(ssid, pass, WIFI_CONNECT_TIMEOUT_SLEEP);
}

// Reading with no connectivity. There's no connection, but we still take a reading and put it
// into the buffer. That's what the buffer exists for: with an hourly cycle, an overnight router outage
// would otherwise wipe a whole night of data.
//
// The config comes from the NVS cache: calibration and schedule are there from the last
// successful sync. Returns moisture (-1 = the sensor gave no reading); offline
// watering is decided based on it.
static int bufferOfflineReading() {
  rtcFailStreak++;
  Serial.printf("Wi-Fi не поднялся (неудач подряд: %lu), буфер: %d из %d\n",
                (unsigned long)rtcFailStreak, readingsCount(),
                READINGS_CAPACITY);

  SoilWindow w = collectWindow(g_soil.soilAvgCount, g_soil.soilEveryMs);
  if (!w.sensorOk) return -1;

  SoilReadingPayload r;
  r.raw       = w.raw;
  r.percent   = w.percent;
  r.voltage   = w.voltage;
  r.minRaw    = w.minRaw;
  r.maxRaw    = w.maxRaw;
  r.rssi      = 0;                           // no network, so RSSI doesn't exist
  r.uptimeSec = millis() / 1000;
  r.bootCount = rtcBootCount;
  readingsPush(r);
  return w.percent;
}

// Common exit to web mode when the buffer is full: there's nowhere left to accumulate, and
// each subsequent cycle would evict one old reading. true = time to
// exit (the caller does return false).
//
// printf with \r\n, not println: before this was moved into a function there was a println here,
// and the Serial output bytes must match the previous ones.
static bool exitToWebIfBufferFull(const char* reason) {
  if (!readingsFull()) return false;

  saveIsActive(false);
  rtcFailStreak = 0;
  Serial.printf(">>> Буфер заполнен, %s — выхожу в обычный режим.\r\n", reason);
  blinkStop();
  return true;
}

// Emergency reboot. In the active cycle the regular boot log isn't sent:
// there are hundreds of timer wake-ups per day, and logging each one is pointless.
// But BROWNOUT/PANIC/WDT are a different matter: they're the only trace that the
// board crashed rather than woke up normally. BROWNOUT is especially important: it
// means a supply voltage sag when the radio turns on and tends to recur.
static void logBootFailureIfAny(ReportBatch& batch, float batteryVolt) {
  if (!bootReasonIsFailure()) return;

  char extra[200];
  snprintf(extra, sizeof(extra),
           "\"resetReason\":\"%s\",\"firmwareVersion\":\"%s\",\"freeHeap\":%u,"
           "\"batteryVolt\":%.2f",
           resetReasonStr(), FW_VERSION, ESP.getFreeHeap(), batteryVolt);
  batch.addLog("error", "boot", extra);
}

// Cell protection with hysteresis. true = Wi-Fi must not be turned on, sleep.
//
// Below BATTERY_LOW_VOLTS the flag is set, above BATTERY_RESUME_VOLTS it's cleared,
// in between it keeps its previous value: after cutoff the board waits for charging and
// doesn't turn on Wi-Fi as soon as the cell recovers a couple hundredths of a volt during sleep.
//
// An unreliable reading (percent = -1: J2 removed or the divider didn't turn on)
// doesn't touch the flag or stop the cycle; we work as usual. The cell
// is then protected from deep discharge only by the TP4056.
static bool lowBatteryHold(const BatteryLogPayload& bat) {
  if (bat.percent < 0) return false;

  if (bat.volt < BATTERY_LOW_VOLTS)          rtcLowBattery = 1;
  else if (bat.volt >= BATTERY_RESUME_VOLTS) rtcLowBattery = 0;
  return rtcLowBattery != 0;
}

// Sleeps due to a low cell: sent as one log once the cell is charged and
// connectivity is back. As with wifi_failed, the moment itself can't be sent: Wi-Fi
// is deliberately off at that time.
static void logLowBatterySleeps(ReportBatch& batch) {
  if (rtcLowBatSleeps == 0) return;

  char extra[160];
  snprintf(extra, sizeof(extra),
           "\"sleeps\":%lu,\"sleepSeconds\":%lu,\"lowVolts\":%.2f,"
           "\"resumeVolts\":%.2f",
           (unsigned long)rtcLowBatSleeps,
           (unsigned long)BATTERY_LOW_SLEEP_SECONDS,
           BATTERY_LOW_VOLTS, BATTERY_RESUME_VOLTS);
  batch.addLog("warn", "battery_low_sleep", extra);
}

// Accumulated connectivity failures: sent as one log once there is a connection.
// The "no Wi-Fi" moment itself can't be sent; hence the counter in RTC.
static void logMissedCycles(ReportBatch& batch) {
  if (rtcFailStreak == 0) return;

  char extra[96];
  snprintf(extra, sizeof(extra), "\"missedCycles\":%lu",
           (unsigned long)rtcFailStreak);
  batch.addLog("warn", "wifi_failed", extra);
}

// Readings using the CURRENT config. Until all soilAvgCount readings are taken,
// the board doesn't send data or go to sleep.
//
// The result is returned in out and is NOT written to flash: a reading goes into the NVS buffer
// only if sending fails (see sendBufferedReports). That way a normal
// cycle (which is almost every cycle) doesn't use up flash endurance at all.
// Returns false if the sensor is disconnected and there's no reading.
static bool measureReading(ReportBatch& batch, SoilReadingPayload& out) {
  Serial.printf("Замеры: %d шт с паузой %lu мс\n",
                g_soil.soilAvgCount, (unsigned long)g_soil.soilEveryMs);
  SoilWindow w = collectWindow(g_soil.soilAvgCount, g_soil.soilEveryMs);

  // Sensor disconnected: don't send data (percent=0 would look like "the soil
  // has dried out" and trigger needless watering), but log the event:
  // otherwise the missing data would go unexplained.
  if (!w.sensorOk) {
    char extra[220];
    snprintf(extra, sizeof(extra),
             "\"raw\":%d,\"minRaw\":%d,\"maxRaw\":%d,\"spread\":%d,"
             "\"retries\":%d,\"validRange\":\"%d..%d\",\"maxSpread\":%d",
             w.raw, w.minRaw, w.maxRaw, w.maxRaw - w.minRaw,
             SOIL_RETRY_ATTEMPTS, SOIL_RAW_MIN_VALID, SOIL_RAW_MAX_VALID,
             SOIL_DISCONNECT_SPREAD);
    batch.addLog("error", "sensor_disconnected", extra);
    return false;
  }

  out.raw       = w.raw;
  out.percent   = w.percent;
  out.voltage   = w.voltage;
  out.minRaw    = w.minRaw;
  out.maxRaw    = w.maxRaw;
  out.rssi      = WiFi.RSSI();
  out.uptimeSec = millis() / 1000;
  out.bootCount = rtcBootCount;
  return true;
}

// Cycle summary. awakeMs is the main battery consumption metric: if it
// creeps up (the network takes longer to come up), the battery will drain sooner.
static void logCycleDone(ReportBatch& batch, uint32_t awakeMs,
                         const BatteryLogPayload& bat,
                         const BatteryLogPayload& pumpBat, bool watered) {
  char extra[400];
  snprintf(extra, sizeof(extra),
           "\"freeHeap\":%u,\"minFreeHeap\":%u,\"awakeMs\":%lu,\"rssi\":%d,"
           "\"batteryVolt\":%.2f,\"batteryPct\":%d,\"buffered\":%d,"
           "\"nvsWrites\":%lu,\"nvsWearPct\":%d,"
           "\"pumpBatteryVolt\":%.2f,\"pumpBatteryPct\":%d,\"watered\":%s",
           ESP.getFreeHeap(), ESP.getMinFreeHeap(),
           (unsigned long)awakeMs, WiFi.RSSI(), bat.volt, bat.percent,
           readingsCount(),
           (unsigned long)readingsWriteCount(), readingsWearPercent(),
           pumpBat.volt, pumpBat.percent, watered ? "true" : "false");
  batch.addLog("info", "cycle_done", extra);
}

// A separate warning when a wear threshold is crossed. cycle_done always
// has the figure, but it would drown in the stream of routine logs, and at 50% and
// 80% of endurance the event needs to be visible via a level filter.
//
// We compare wear before and after the cycle instead of checking for equality with the threshold:
// the counter can jump past a round number in one cycle, and the event
// would be lost. The state is kept in RTC: it survives deep sleep, while
// storing it in NVS would mean writing flash to track flash wear.
static void logWearThresholds(ReportBatch& batch) {
  int wearNow = readingsWearPercent();
  if (wearNow <= (int)rtcWearReported) return;

  static const int THRESHOLDS[] = {50, 80, 90, 95};
  for (unsigned i = 0; i < sizeof(THRESHOLDS) / sizeof(THRESHOLDS[0]); i++) {
    int t = THRESHOLDS[i];
    if (wearNow >= t && (int)rtcWearReported < t) {
      char w[220];
      snprintf(w, sizeof(w),
               "\"nvsWrites\":%lu,\"nvsWearPct\":%d,\"threshold\":%d,"
               "\"endurance\":%lu,\"note\":\"оценка сверху, wear leveling не учтён\"",
               (unsigned long)readingsWriteCount(), wearNow, t,
               NVS_ENDURANCE);
      batch.addLog(t >= 80 ? "error" : "warn", "nvs_wear", w);
    }
  }
  rtcWearReported = wearNow;
}

// Report on NVS usage by the readings buffer: a log for every counter increment.
// It can't be sent at the moment of the write itself: push happens on a connectivity failure, and
// drop happens after the batch logs have already gone out. So with the first batch while
// connected we send the delta from the last reported value.
//
// Returns a counter snapshot. The caller writes it to rtcWritesReported
// ONLY after a successful send: a log lost together with a failed report
// would otherwise never be repeated. A duplicate on partial delivery is less harmful than
// a miss.
static uint32_t logNvsWrites(ReportBatch& batch) {
  uint32_t now = readingsWriteCount();
  if (rtcWritesReported > now) rtcWritesReported = now;  // the counter was reset
  if (now > rtcWritesReported) {
    char w[128];
    snprintf(w, sizeof(w),
             "\"scope\":\"buffer\",\"nvsWrites\":%lu,\"delta\":%lu,"
             "\"nvsWearPct\":%d",
             (unsigned long)now, (unsigned long)(now - rtcWritesReported),
             readingsWearPercent());
    batch.addLog("info", "nvs_write", w);
  }
  return now;
}

// Sending. Readings go out in chunks of READINGS_SEND_CHUNK: a body for the whole
// buffer is several kilobytes, and with a weak signal such a POST is more likely
// to time out than get through.
//
// fresh is the current cycle's reading (nullptr if the sensor is disconnected). It goes to flash
// ONLY on failure: a reading confirmed by the server goes straight
// from RAM, and a normal cycle doesn't write NVS at all; flash endurance is spent on connectivity
// failures, not on every wake-up.
//
// Logs and waterings go with the first chunk; after that, readings only. rep is an output:
// the caller needs haveDeviceInfo/isActive/revision for mid-cycle
// deactivation. batchDelivered = the first chunk (and with it the logs and waterings from
// batch) was accepted by the server, even if one of the later ones didn't get through.
static ApiResult sendBufferedReports(const ReportBatch& batch,
                                     ReportOutcome& rep,
                                     const SoilReadingPayload* fresh,
                                     bool& batchDelivered) {
  ApiResult repRes;
  bool logsSent   = false;
  bool anyRequest = false;   // whether there was at least one request; see below
  batchDelivered  = false;

  // A non-empty buffer is drained with several requests, and the series can
  // break off at any of them; for that time the fresh reading is safer moved in with
  // the rest. Writing to flash here isn't wasteful: the buffer is non-empty, so a failure
  // has already happened, and this is exactly the case the buffer exists for.
  if (fresh && !readingsEmpty()) {
    readingsPush(*fresh);
    fresh = nullptr;
  }

  while (true) {
    ReportBatch send;

    // The first pass takes the accumulated logs along with the readings.
    bool firstPass = !logsSent;
    if (!logsSent) {
      send = batch;
      logsSent = true;
    }

    int n = readingsFillBatch(send);

    // Buffer empty (normal cycle): the fresh reading goes straight from RAM.
    bool freshInSend = false;
    if (fresh && n == 0) {
      send.addReading(*fresh);
      freshInSend = true;
    }

    if (n == 0 && !freshInSend && send.logs() == 0) break;   // nothing to send

    repRes = apiReport(g_dev, send, rep);
    anyRequest = true;

    if (!repRes.ok()) {
      Serial.printf("Report не прошёл — %d замеров остаются в буфере\n",
                    readingsCount() + (fresh ? 1 : 0));
      break;
    }

    if (firstPass) batchDelivered = true;

    // Count as delivered ONLY what's confirmed. A single HTTP 200 isn't enough:
    // the batch may have been partially accepted, and then data would vanish silently.
    if (!rep.readingsFullyAccepted()) {
      Serial.printf("Сервер отверг %d замеров из %d — оставляю в буфере\n",
                    rep.readingsRejected,
                    rep.readingsAccepted + rep.readingsRejected);
      break;
    }

    if (n > 0) readingsDropSent(n);
    if (freshInSend) fresh = nullptr;   // confirmed, flash wasn't needed

    if (readingsEmpty() && !fresh) break;
  }

  // The fresh reading wasn't confirmed (connection lost or server rejected it); now
  // the safeguard is needed: put it into NVS, the next cycle will send it.
  if (fresh) readingsPush(*fresh);

  // There was nothing to send, so there was no failure either: sync succeeded, there is
  // connectivity. The separate check is needed because with zero requests repRes
  // stays at the default NoWifi and would look like a failure.
  if (!anyRequest) repRes.status = ApiStatus::Ok;

  return repRes;
}

// ACTIVE CYCLE (isActive == true). Called at the start of setup() on every
// wake-up. The order of operations is the key thing in this function:
//
//   BOOT -> Wi-Fi -> sync -> isActive check -> readings -> report -> sleep
//
// The config is fetched BEFORE the readings because it determines how many readings
// to take, with what pause, and which calibration to compute percentages with.
// Returns true if we went to sleep (the regular setup() is no longer needed).
//
// The function itself is control logic only: the linear work of the stages is moved
// into the static functions above, and here all branches and all exit paths are visible.
bool activeCycle() {
  if (!g_dev.isActive) return false;         // inactive: regular boot

  // 1. Emergency stop. First thing, before any networking: the button must
  // work even when the server is unreachable.
  if (emergencyStopRequested()) {
    return false;                            // the regular setup() continues
  }

  uint32_t cycleStart = millis();

  // Blink once per second for the whole time awake: from the outside the active
  // cycle is otherwise indistinguishable from sleep; the LED is off in both cases.
  //
  // Runs on a hardware timer, so it keeps blinking even during the reading
  // windows that block the main thread for tens of seconds.
  // Turned off in goToSleep(), i.e. on any exit from the cycle.
  blinkStart();

  Serial.printf("\n=== АКТИВНЫЙ ЦИКЛ: пробуждение #%lu ===\n",
                (unsigned long)rtcBootCount);

  soilBegin();
  batteryBegin();

  // 2. Battery FIRST, before turning on the radio and before powering the sensor.
  //
  // The order is essential: at peak the Wi-Fi transmitter draws hundreds of
  // milliamps and pulls down the battery voltage. A reading under load
  // would show not the charge but the depth of the sag, and the board would decide the
  // cell is running low when it's full.
  //
  // Settings come from the NVS cache: before sync there's nowhere else to get them, and sync itself
  // is impossible without the radio. In practice this isn't a problem: checkEveryMs
  // rarely changes, and one cycle on the old value doesn't break anything.
  BatteryLogPayload bat = readBatteryForSync();

  // 2a. Cell protection; see lowBatteryHold(). Triggers only when the
  // cell is low; otherwise the cycle proceeds as usual. No soil reading
  // is taken in that case: the window keeps the sensor and CPU on for seconds,
  // and the point right now is to hold out until charging.
  if (lowBatteryHold(bat)) {
    rtcLowBatSleeps++;
    Serial.printf(">>> Банка %.2f В: ниже %.2f В Wi-Fi не включаю, жду зарядки "
                  "выше %.2f В. Сон %lu с (подряд: %lu)\n",
                  bat.volt, BATTERY_LOW_VOLTS, BATTERY_RESUME_VOLTS,
                  (unsigned long)BATTERY_LOW_SLEEP_SECONDS,
                  (unsigned long)rtcLowBatSleeps);
    goToSleep(BATTERY_LOW_SLEEP_SECONDS);    // does not return
  }

  // 2b. Pump cell, also before the radio. The voltage divider on GPIO35 is enabled only
  // for the window. The reading is needed both by the dashboard ("Pump battery") and by watering:
  // below g_pump.minBatteryVolt the pump isn't turned on.
  BatteryLogPayload pumpBat = readPumpBatteryForSync();

  // 3. Wi-Fi; see wifiConnectFromNvs(). If it didn't come up, we still take a reading
  // and put it into the buffer (bufferOfflineReading), then go back to sleep.
  // Offline watering uses the NVS cache (autoWaterStep with online = false).
  if (!wifiConnectFromNvs()) {
    int offlinePercent = bufferOfflineReading();
    bool wateredOffline = autoWaterStep(offlinePercent, pumpBat, false, nullptr);

    // Exit to web mode only when the buffer is full. There used to be a
    // failure counter here (5 cycles), but with sleepSeconds=3600 that would mean
    // the board brings up the access point after five hours and keeps Wi-Fi on
    // continuously, draining the battery. Now the board keeps sleeping and accumulating
    // while there's room, and gives up when data starts getting lost: at the very
    // transition, the push inside bufferOfflineReading has already evicted one old
    // reading; this is deliberate, a fresh one is worth more than the evicted one (see readingsPush).
    if (exitToWebIfBufferFull("связи нет")) return false;
    goToSleep(wateredOffline ? g_pump.postWaterSleepSeconds
                             : g_dev.sleepSeconds);   // does not return
  }

  // 4. Config BEFORE the readings, in one request. If it didn't arrive, we work from the
  // NVS cache but still take readings: approximate data is better
  // than none.
  //
  // The "we were deleted from the server, re-register" branch is no longer here:
  // sync with an unknown chipId creates the device itself, so a database wipe
  // and first power-on follow the same path.
  SyncOutcome sync;
  ApiResult syncRes = apiSync(g_dev, g_soil, g_bat, g_pump, bat, pumpBat, sync);

  if (!syncRes.ok()) {
    Serial.printf("Sync не прошёл (%s, код %d) — беру значения из кэша NVS\n",
                  syncRes.cstr(), syncRes.httpCode);

    // Wi-Fi is up but the server isn't: that's also a connectivity failure, and it must be counted
    // the same as Wi-Fi failing to come up.
    //
    // Readings continue meanwhile: they go into the buffer and get sent when the server
    // comes back. Exit to web mode happens when the buffer is full, same as above.
    rtcFailStreak++;
    Serial.printf("Неудач связи подряд: %lu, буфер: %d из %d\n",
                  (unsigned long)rtcFailStreak, readingsCount(),
                  READINGS_CAPACITY);

    if (exitToWebIfBufferFull("сервер недоступен")) return false;
  }

  // 5. isActive check AFTER syncing: the server may have deactivated the
  // board while it was asleep. Fail-safe: anything other than an explicit true is inactive.
  //
  // Only on a successful sync: on a connection failure g_dev.isActive stays as loaded from
  // NVS, and exiting to web mode because of a single failed request is wrong;
  // that's what the rtcFailStreak counter is for.
  //
  // exitToWebIfBufferFull doesn't fit here: activity was already cleared BY THE SERVER
  // (it's in g_dev), the whole config must be saved, and the buffer has nothing to do with it.
  if (syncRes.ok() && !g_dev.isActive) {
    Serial.println(">>> Сервер снял активность — выхожу в обычный режим.");
    markConfigApplied(sync, nullptr);
    saveDeviceConfig();
    blinkStop();
    return false;                            // the regular setup() continues
  }

  // 6. One batch for the whole cycle. Everything that used to go out in separate requests
  // (boot, wifi_failed, config_applied, reading, cycle_done) is collected
  // here and goes out in one report at the end.
  ReportBatch batch;
  markConfigApplied(sync, &batch);
  logBootFailureIfAny(batch, bat.volt);
  logMissedCycles(batch);
  logLowBatterySleeps(batch);

  // 7-8. Readings using the current config; see measureReading(). The fresh reading
  // stays in RAM: it goes into the NVS buffer only if sending fails;
  // a normal cycle doesn't touch flash.
  SoilReadingPayload fresh;
  bool haveFresh = measureReading(batch, fresh);

  // 8a. Auto-watering; see watering.h. With a live sync the board asks the server
  // before watering, otherwise it waters from the cache. Everything awaiting sending (this and
  // previous waterings, the check reading, offline skips) goes into the batch.
  bool watered = autoWaterStep(haveFresh ? fresh.percent : -1, pumpBat,
                               syncRes.ok(), &batch);
  WateringSent wateringSent = wateringAddPending(batch);

  // 9. Cycle summary. awake is captured BEFORE sending and printed at the very end
  // with the same value, as before.
  uint32_t awake = millis() - cycleStart;
  logCycleDone(batch, awake, bat, pumpBat, watered);
  logWearThresholds(batch);
  uint32_t writesLogged = logNvsWrites(batch);

  // 10. Sending in chunks; see sendBufferedReports(). rep is needed below for
  // mid-cycle deactivation.
  ReportOutcome rep;
  bool batchDelivered;
  ApiResult repRes = sendBufferedReports(batch, rep,
                                         haveFresh ? &fresh : nullptr,
                                         batchDelivered);

  // Waterings and the check reading went out with the first chunk, so they can be removed
  // from RTC even if one of the later reading chunks didn't get through.
  if (batchDelivered) wateringMarkDelivered(wateringSent);

  if (repRes.ok()) {
    // Reset the failure counter only now: if it were reset earlier, a
    // failed report would lose the information about missed cycles.
    // The low-cell sleep counter too, for the same reason.
    rtcFailStreak   = 0;
    rtcLowBatSleeps = 0;

    // The nvs_write log was delivered: record up to which counter value
    // usage has been reported. Writes that happened during the send itself (drop
    // of chunks, push on partial rejection) aren't in the snapshot and will go out
    // next cycle.
    rtcWritesReported = writesLogged;

    // Mid-cycle deactivation: while readings were being taken (~20 s), the server may have cleared
    // activity. Catch it before sleep, not on the next wake-up.
    if (rep.haveDeviceInfo && !rep.isActive) {
      Serial.println(">>> Сервер снял активность во время цикла — выхожу в обычный режим.");
      g_dev.isActive = false;
      saveDeviceConfig();
      blinkStop();
      return false;
    }

    // The revision changed while the cycle was running. Sleeping on a stale sleepSeconds
    // isn't a big deal (we'll be off by at most one cycle), but it needs flagging:
    // the next sync will fetch the current values.
    if (rep.haveDeviceInfo && rep.revision != g_dev.revision)
      Serial.printf("Конфиг изменился во время цикла (rev %d -> %d) — заберу на следующем пробуждении\n",
                    g_dev.revision, rep.revision);
  } else {
    // Report failed with Wi-Fi up: that's also a connectivity failure. But if the streak
    // was already incremented by a sync failure in this same cycle, don't count it again:
    // it's one missed cycle, not two.
    if (syncRes.ok()) rtcFailStreak++;
    Serial.printf("Неудач связи подряд: %lu, буфер: %d из %d\n",
                  (unsigned long)rtcFailStreak, readingsCount(),
                  READINGS_CAPACITY);

    // Same safeguard as in the "no Wi-Fi" and "no sync" branches. Without it, a board
    // with a full buffer and a failing report would spin in the active cycle
    // forever, evicting one old reading on every wake-up:
    // sendBufferedReports() has already moved the unsent reading into the buffer, and
    // the next cycle would start losing data.
    if (exitToWebIfBufferFull("report не проходит")) return false;
  }

  Serial.printf("Цикл занял %lu мс\n", (unsigned long)awake);

  // After watering, a short sleep until the check reading, then the normal rhythm.
  goToSleep(watered ? g_pump.postWaterSleepSeconds
                    : g_dev.sleepSeconds);   // does not return
  return true;
}

void setup() {
  // Release the hold on the sensor and voltage divider pins left over from sleep, before
  // any work with them (soilBegin/batteryBegin). See sleepPinsRelease().
  sleepPinsRelease();

  Serial.begin(115200);
  delay(500);

  // RTC block first: it may be left over from the previous wake-up or contain
  // garbage after a power reset.
  rtcInitIfNeeded();
  rtcBootCount++;

  // Config from NVS before any logic; everything after works from g_dev / g_soil.
  // If the server has never responded yet, these will be the defaults from config.h.
  loadDeviceConfig();
  loadSoilConfig();
  loadBatteryConfig();
  loadPumpConfig();
  readingsBegin();                      // buffer of unsent readings
  migrateLegacyCollectFlag();           // one-time migration from collect/on

  g_dev.chipId = chipId();

  Serial.printf("\n=== Загрузка #%lu | причина: %s | fw %s ===\n",
                (unsigned long)rtcBootCount, resetReasonStr(), FW_VERSION);
  Serial.printf("chipId: %s | id: %s | active=%s\n",
                g_dev.chipId.c_str(),
                g_dev.id.length() ? g_dev.id.c_str() : "(ещё не синхронизировались)",
                g_dev.isActive ? "да" : "нет");
  Serial.printf("Почва: dry=%d wet=%d every=%lu avg=%d\n",
                g_soil.soilDry, g_soil.soilWet,
                (unsigned long)g_soil.soilEveryMs, g_soil.soilAvgCount);
  Serial.printf("Автополив: %s | при <= %d%% | %d мл / %.2f мл/с | часы платы %lu с | "
                "в очереди %lu\n",
                g_pump.autoWaterEnabled ? "вкл" : "выкл", g_soil.autoWaterPercent,
                g_pump.waterMl, g_pump.flowMlPerSec, (unsigned long)clockNow(),
                (unsigned long)rtcWaterQueueLen);

  // Active cycle: if the board is active, it goes to sleep from here and the regular
  // setup() won't be needed.
  if (activeCycle()) return;

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);          // off until connected

  // 11 dB = full input range (~0..3.1 V), otherwise the sensor would hit the ceiling
  soilBegin();                     // ADC input and power pin setup
  batteryBegin();                  // battery reading on GPIO34
  readSoilOnce();                  // first value right away so the page isn't empty
  Serial.printf("Датчик почвы на GPIO%d: %d%% (сырое АЦП: %d)\n",
                PIN_SOIL, soilPercent, soilRaw);

  WiFi.mode(WIFI_AP_STA);
  startAP();

  Serial.println("\n=== ТОЧКА ДОСТУПА ===");
  Serial.printf("Сеть:   %s / %s\n", AP_SSID, AP_PASS);
  Serial.printf("Адрес:  http://%s\n", WiFi.softAPIP().toString().c_str());

  // Auto-connect to the saved network, if there is one in flash.
  bool connected = connectSaved();

  // scan the air only if we did NOT connect, i.e. the portal is actually needed.
  // If connected, the scan starts lazily on the first page visit.
  if (!connected) startScan();

  webBegin();                      // routes and web server start

  // First sync and boot log, only if the board is on the router's network.
  // Without a network this is deferred until the first successful connection (see loop).
  if (connected) syncWithServer();
}

// First contact after a regular boot: config + boot log.
//
// Registration as a separate step is gone: sync with an unknown chipId
// creates the device on the server itself. Hence there's also no "id is stale, database
// wiped" branch here: a database wipe is indistinguishable from first power-on.
void syncWithServer() {
  static bool bootLogged = false;

  ReportBatch batch;
  bool ok = syncConfig(&batch);

  // The boot log is sent once per session. resetReason is especially valuable:
  // BROWNOUT points to weak power, PANIC to a crash that would otherwise go
  // unseen (the board just reboots).
  if (!bootLogged && ok) {
    BatteryReading b = batteryLast();
    char extra[220];
    snprintf(extra, sizeof(extra),
             "\"resetReason\":\"%s\",\"firmwareVersion\":\"%s\",\"freeHeap\":%u,"
             "\"batteryVolt\":%.2f,\"batteryPct\":%d",
             resetReasonStr(), FW_VERSION, ESP.getFreeHeap(),
             b.voltage, b.percent);
    // BROWNOUT and PANIC are failures, not a routine boot. At info level they
    // would drown in the stream of normal boots; at error they're visible via a filter.
    batch.addLog(bootReasonIsFailure() ? "error" : "info", "boot", extra);
  }

  if (!batch.empty()) {
    ReportOutcome rep;
    if (apiReport(g_dev, batch, rep).ok()) bootLogged = true;
  }
}

// INACTIVE MODE: heartbeat + config sync every deviceLogsSeconds.
//
// Sync goes along with the heartbeat for a reason: this is how the board learns about
// isActive=true set from the server, and about new settings, without
// waiting for a reboot.
void pollHeartbeat() {
  static unsigned long last = 0;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - last < g_dev.deviceLogsSeconds * 1000UL) return;
  last = millis();

  ReportBatch batch;
  if (!syncConfig(&batch)) return;

  // The server enabled activity: go to the active cycle without waiting for a
  // reboot. Save to NVS first, then sleep: otherwise the board would
  // wake up in a state it doesn't know about itself.
  if (g_dev.isActive) {
    Serial.println(">>> Сервер включил активность — перехожу в активный режим.");
    saveDeviceConfig();
    batch.addLog("info", "activated", "\"source\":\"server\"");
    ReportOutcome rep;
    apiReport(g_dev, batch, rep);
    delay(200);
    goToSleep(g_dev.sleepSeconds);        // does not return
  }

  BatteryReading b = batteryLast();
  char extra[340];
  snprintf(extra, sizeof(extra),
           "\"freeHeap\":%u,\"rssi\":%d,\"ip\":\"%s\",\"isActive\":false,"
           "\"soilPercent\":%d,\"batteryVolt\":%.2f,\"batteryPct\":%d,"
           "\"buffered\":%d,\"nvsWrites\":%lu,\"nvsWearPct\":%d",
           ESP.getFreeHeap(), WiFi.RSSI(),
           WiFi.localIP().toString().c_str(),
           soilPercent, b.voltage, b.percent, readingsCount(),
           (unsigned long)readingsWriteCount(), readingsWearPercent());
  batch.addLog("info", "heartbeat", extra);

  // Drain the buffer here too. Otherwise a board that dropped into inactive mode
  // because the buffer overflowed would never send it: the active cycle
  // with its sending no longer runs, and the accumulated data would be stuck
  // forever.
  //
  // One chunk per heartbeat, not everything at once: inactive mode isn't
  // in a hurry, and a long series of requests would hang the web server, which in
  // this mode is the main thing.
  int n = readingsFillBatch(batch);

  // Unreported NVS usage (e.g. drop of the previous chunk), in the same batch.
  uint32_t writesLogged = logNvsWrites(batch);

  // Waterings that didn't reach the server while the board was active (e.g.
  // done offline before dropping into this mode) also go from here.
  WateringSent wateringSent = wateringAddPending(batch);

  ReportOutcome rep;
  if (apiReport(g_dev, batch, rep).ok()) {
    rtcWritesReported = writesLogged;
    wateringMarkDelivered(wateringSent);
    if (n > 0) {
      if (rep.readingsFullyAccepted()) readingsDropSent(n);
      else Serial.printf("Сервер отверг %d замеров — оставляю в буфере\n",
                         rep.readingsRejected);
    }
  }
}

// Config sync with the server. MAIN INVARIANT: call BEFORE any
// actions: readings, sleep, sending. revision can change at any
// moment, and working with a stale calibration or a stale schedule
// is not allowed: percentages would be wrong, and sleep the wrong length.
//
// batch != nullptr -> the config_applied log goes into it if the config
// changed. Returns false if the config couldn't be fetched; that's no
// reason to stop: the caller continues from the NVS cache.
bool syncConfig(ReportBatch* batch) {
  // Battery is NOT sent from here, only from the active cycle.
  //
  // This path is called every deviceLogsSeconds (currently 10 s), i.e.
  // 8640 records per day versus 144 in active mode with
  // sleepSeconds=600; the database would fill up with junk within a day.
  //
  // percent stays -1, and by that sign apiSync doesn't add
  // batteryLogs to the request body at all.
  BatteryLogPayload bat;
  BatteryLogPayload pumpBat;
  pumpBat.type = "pump";

  SyncOutcome sync;
  ApiResult res = apiSync(g_dev, g_soil, g_bat, g_pump, bat, pumpBat, sync);

  if (!res.ok()) {
    Serial.printf("Sync не прошёл (%s, код %d) — работаю по кэшу\n",
                  res.cstr(), res.httpCode);
    return false;
  }

  markConfigApplied(sync, batch);
  return true;
}

// Retry of the FIRST sync if there was no network at startup. Needed only
// until the board first reaches the server and gets an id:
// after that pollHeartbeat() takes over, which syncs every
// deviceLogsSeconds and survives temporary outages on its own.
//
// Hence the exit condition on a non-empty id: it's not "all good now" but
// "first contact happened, the rest isn't my concern".
void retrySync() {
  static unsigned long last = 0;
  if (WiFi.status() != WL_CONNECTED) return;
  if (g_dev.id.length()) return;              // first contact already happened
  if (millis() - last < 60000) return;
  last = millis();
  syncWithServer();
}

void loop() {
  webLoop();
  pollScan();
  retrySync();
  // auto-scan removed: it's only needed on the setup page, not all the time.
  // The list refreshes when the portal is opened and via the "Refresh" button.

  manageAP();
  pollSoil();
  pollHeartbeat();
  pollBattery();                   // battery charge, interval = soilEveryMs
}
