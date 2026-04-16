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
