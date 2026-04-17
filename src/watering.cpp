#include "watering.h"
#include "config.h"
#include "state.h"
#include <esp_system.h>

static const uint32_t DAY_SECONDS = 24UL * 3600;

// How many consecutive samples (10 ms each) the BOOT button must be held to
// stop the pump. The motor induces noise on the common ground, and a single LOW on
// GPIO0 while the pump runs isn't a press yet.
static const int STOP_PRESS_SAMPLES = 5;

// ---------- pump ----------

// Turns the pump on for ms milliseconds. Blocking: in the active cycle there's nothing else
// to do, and the LED blinking runs off a hardware timer. The BOOT button
// stops the pump early — a manual stop in case the hose falls out.
// Returns how long the pump actually ran.
static uint32_t pumpRun(uint32_t ms, bool& stopped) {
  stopped = false;
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  pinMode(PIN_PUMP, OUTPUT);

  Serial.printf(">>> НАСОС: включаю на %.1f с (держи BOOT — стоп)\n", ms / 1000.0f);
  Serial.flush();

  uint32_t start = millis();
  int pressed = 0;
  digitalWrite(PIN_PUMP, HIGH);
  while (millis() - start < ms) {
    pressed = digitalRead(PIN_BOOT_BTN) == LOW ? pressed + 1 : 0;
    if (pressed >= STOP_PRESS_SAMPLES) { stopped = true; break; }
    delay(10);
  }
  digitalWrite(PIN_PUMP, LOW);

  uint32_t ran = millis() - start;
  Serial.printf(">>> НАСОС: выключен через %.2f с%s\n", ran / 1000.0f,
                stopped ? " (остановлен кнопкой BOOT)" : "");
  return ran;
}

// ---------- daily limit ----------

static int wateringsLast24h() {
  uint32_t now = clockNow();
  int n = 0;
  for (int i = 0; i < WATERING_RING_SIZE; i++) {
    uint32_t t = rtcWaterTimes[i];
    if (t != 0 && now - t < DAY_SECONDS) n++;
  }
  return n;
}

static void ringPush(uint32_t t) {
  rtcWaterTimes[rtcWaterTimesPos % WATERING_RING_SIZE] = t ? t : 1;   // 0 = empty
  rtcWaterTimesPos = (rtcWaterTimesPos + 1) % WATERING_RING_SIZE;
}

// ---------- send queue ----------

static void queuePush(const WateringRecord& w) {
  if (rtcWaterQueueLen >= WATERING_QUEUE_SIZE) {
    // The oldest watering has gone unsent for a long time — the fresh one is more valuable.
    Serial.println("Очередь поливов полна — вытесняю самый старый");
    memmove(&rtcWaterQueue[0], &rtcWaterQueue[1],
            sizeof(WateringRecord) * (WATERING_QUEUE_SIZE - 1));
    rtcWaterQueueLen = WATERING_QUEUE_SIZE - 1;
  }
  rtcWaterQueue[rtcWaterQueueLen++] = w;
}

// Follow-up reading: for a watering still in the queue — straight into the record (it goes out with
// it); for one already sent — as a separate wateringsAfter.
static void noteMoistureAfter(uint32_t nonce, int percent) {
  for (uint32_t i = 0; i < rtcWaterQueueLen; i++) {
    if (rtcWaterQueue[i].nonce == nonce) {
      rtcWaterQueue[i].moistureAfter = percent;
      return;
    }
  }
  rtcAfterNonce    = nonce;
  rtcAfterMoisture = percent;
}

// ---------- skips ----------

// Dry, but watering isn't allowed. With the server — log right away; offline — a counter in
// RTC that goes out as a single log in the very first report (see wateringAddPending).
static void reportSkip(const char* reason, const char* source, int percent,
                       const BatteryLogPayload& pumpBat, ReportBatch* batch) {
  Serial.printf("Полив пропущен (%s, решил %s): влажность %d%% <= %d%%, банка насоса %.2f В\n",
                reason, source, percent, g_soil.autoWaterPercent, pumpBat.volt);
  if (!batch) {
    rtcOfflineSkips++;
    strlcpy(rtcOfflineSkipReason, reason, sizeof(rtcOfflineSkipReason));
    return;
  }

  bool battery = strncmp(reason, "pump_battery", 12) == 0;
  char extra[260];
  snprintf(extra, sizeof(extra),
           "\"reason\":\"%s\",\"source\":\"%s\",\"moisture\":%d,\"autoWaterPercent\":%d,"
           "\"pumpBatteryVolt\":%.2f,\"minBatteryVolt\":%.2f,\"wateringsToday\":%d,"
           "\"maxWateringsPerDay\":%d",
           reason, source, percent, g_soil.autoWaterPercent, pumpBat.volt,
           g_pump.minBatteryVolt, wateringsLast24h(), g_pump.maxWateringsPerDay);
  batch->addLog(battery ? "warn" : "info", "watering_skipped", extra);
}

// ---------- auto-watering step ----------

bool autoWaterStep(int percent, const BatteryLogPayload& pumpBat,
                   bool online, ReportBatch* batch) {
  // The follow-up wake-up is the one RIGHT AFTER watering. We clear the flag in any
  // case: if this wake-up isn't the next one (sleep due to a dead cell,
  // emergency stop), the reading is no longer about that watering.
  bool afterWake = rtcPostWaterNonce != 0 && rtcBootCount == rtcPostWaterBoot + 1;
  if (afterWake) {
    if (percent >= 0) noteMoistureAfter(rtcPostWaterNonce, percent);
    Serial.printf("Контрольный замер после полива: %d%%\n", percent);
  }
  rtcPostWaterNonce = 0;
  rtcPostWaterBoot  = 0;

  // Don't water again right after watering: the water is still spreading through the pot.
  // If it really is dry, the next regular wake-up will water.
  if (afterWake) return false;

  // The sensor gave no reading — don't water: 0 % from a detached sensor would look
  // like parched soil and pour water every cycle.
  if (percent < 0 || !g_pump.autoWaterEnabled) return false;
  if (percent > g_soil.autoWaterPercent) return false;

  // The board's own checks — before asking the server.
  if (pumpBat.percent < 0) {
    reportSkip("pump_battery_invalid", "board", percent, pumpBat, batch);
    return false;
  }
  if (pumpBat.volt < g_pump.minBatteryVolt) {
    reportSkip("pump_battery_low", "board", percent, pumpBat, batch);
    return false;
  }
  if (wateringsLast24h() >= g_pump.maxWateringsPerDay) {
    reportSkip("daily_limit", "board", percent, pumpBat, batch);
    return false;
  }

  // Dose and limits: from the cache, or from its reply when the server is up.
  int32_t ml      = g_pump.waterMl;
  float   flow    = g_pump.flowMlPerSec;
  int32_t maxSec  = g_pump.maxPumpSeconds;
  bool    offline = true;

  if (online) {
    WateringReply r;
    ApiResult res = apiRequestWatering(g_dev, percent, pumpBat.volt, r);
    if (res.ok()) {
      if (!r.allowed) {
        reportSkip(r.reason[0] ? r.reason : "server", "server", percent, pumpBat, batch);
        return false;
      }
      if (r.ml > 0)             ml     = r.ml;
      if (r.flowMlPerSec > 0)   flow   = r.flowMlPerSec;
      if (r.maxPumpSeconds > 0) maxSec = r.maxPumpSeconds;
      offline = false;
    } else {
      Serial.printf("Сервер не ответил на watering (%s) — лью по кэшу\n", res.cstr());
    }
  }

  // Pump run time — computed by us: dose / flow rate, but no longer than
  // maxPumpSeconds and the firmware's absolute ceiling.
  if (flow <= 0.0f) flow = DEF_PUMP_FLOW;
  if (maxSec <= 0 || maxSec > PUMP_ABS_MAX_SECONDS) maxSec = PUMP_ABS_MAX_SECONDS;
  float seconds = ml / flow;
  if (seconds > maxSec) seconds = maxSec;
  if (ml <= 0 || seconds <= 0.0f) return false;

  bool stopped;
  uint32_t ran = pumpRun((uint32_t)lroundf(seconds * 1000.0f), stopped);

  WateringRecord w;
  w.nonce          = esp_random() | 1;   // 0 is reserved for "none"
  w.bootCount      = rtcBootCount;
  w.ml             = ml;
  w.pumpMs         = ran;
  w.flowMlPerSec   = flow;
  w.moistureBefore = percent;
  w.moistureAfter  = -1;
  w.pumpVolt       = pumpBat.volt;
  w.offline        = offline ? 1 : 0;
  w.at             = clockNow();
  queuePush(w);
  ringPush(w.at);

  rtcPostWaterBoot  = rtcBootCount;
  rtcPostWaterNonce = w.nonce;

  if (batch) {
    char extra[260];
    snprintf(extra, sizeof(extra),
             "\"ml\":%d,\"seconds\":%.2f,\"plannedSeconds\":%.2f,\"flowMlPerSec\":%.2f,"
             "\"moisture\":%d,\"autoWaterPercent\":%d,\"pumpBatteryVolt\":%.2f,"
             "\"offline\":%s,\"stoppedByButton\":%s",
             ml, ran / 1000.0f, seconds, flow, percent, g_soil.autoWaterPercent,
             pumpBat.volt, offline ? "true" : "false", stopped ? "true" : "false");
    batch->addLog("info", "watering", extra);
  }
  return true;
}

// ---------- sending ----------

WateringSent wateringAddPending(ReportBatch& batch) {
  WateringSent sent;

  for (uint32_t i = 0; i < rtcWaterQueueLen; i++) batch.addWatering(rtcWaterQueue[i]);
  sent.queued = rtcWaterQueueLen;

  if (rtcAfterNonce != 0) {
    batch.addWateringAfter(rtcAfterNonce, rtcAfterMoisture);
    sent.after = true;
  }

  if (rtcOfflineSkips > 0) {
    char extra[120];
    snprintf(extra, sizeof(extra),
             "\"offline\":true,\"count\":%lu,\"lastReason\":\"%s\"",
             (unsigned long)rtcOfflineSkips, rtcOfflineSkipReason);
    batch.addLog("warn", "watering_skipped", extra);
    sent.skips = true;
  }
  return sent;
}

void wateringMarkDelivered(const WateringSent& sent) {
  if (sent.queued > 0) {
    uint32_t left = rtcWaterQueueLen > (uint32_t)sent.queued
                      ? rtcWaterQueueLen - sent.queued : 0;
    if (left) memmove(&rtcWaterQueue[0], &rtcWaterQueue[sent.queued],
                      sizeof(WateringRecord) * left);
    rtcWaterQueueLen = left;
  }
  if (sent.after) rtcAfterNonce = 0;
  if (sent.skips) {
    rtcOfflineSkips = 0;
    rtcOfflineSkipReason[0] = '\0';
  }
}
