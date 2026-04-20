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

