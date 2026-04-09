#include "soil.h"
#include "config.h"

int soilRaw     = -1;
int soilPercent = -1;

void soilBegin() {
  // 11 dB = full input range (~0..3.1 V), otherwise the sensor hits the ceiling
  analogSetPinAttenuation(PIN_SOIL, ADC_11db);

  // Sensor power is switchable. Start with it off: let it stay
  // de-energized all the time except during the readings themselves.
  pinMode(PIN_SOIL_POWER, OUTPUT);
  digitalWrite(PIN_SOIL_POWER, LOW);
}

void soilPowerOn() {
  digitalWrite(PIN_SOIL_POWER, HIGH);
  delay(SOIL_WARMUP_MS);        // the sensor needs time to settle
}

void soilPowerOff() {
  digitalWrite(PIN_SOIL_POWER, LOW);
}

void readSoilOnce() {
  soilPowerOn();
  readSoil();
  soilPowerOff();
}

void soilPowerCycle() {
  soilPowerOff();
  delay(SOIL_RESET_MS);           // let the electrolytic cap at the LDO output discharge
  soilPowerOn();                  // already includes the SOIL_WARMUP_MS warm-up
}

int soilRawToPercent(int raw) {
  // The scale is inverted (dry = higher), hence dry - raw.
  // The span can turn out to be zero with a bad calibration — then we can't divide.
  int span = g_soil.soilDry - g_soil.soilWet;
  if (span <= 0) return -1;
  long pct = (long)(g_soil.soilDry - raw) * 100 / span;
  return constrain(pct, 0, 100);
}

// One measurement: we average several samples because the ESP32 ADC is noisy
// and adjacent reads wander by tens of units.
void readSoil() {
  const int SAMPLES = 16;
  uint32_t sum = 0;
  for (int i = 0; i < SAMPLES; i++) {
    sum += analogRead(PIN_SOIL);
    delay(2);
  }
  soilRaw = sum / SAMPLES;
  soilPercent = soilRawToPercent(soilRaw);
}

// Verbal assessment — so the page is understandable without reading numbers.
const char* soilWord(int pct) {
  if (pct < 0)  return "нет данных";
  if (pct < 15) return "сухо — пора поливать";
  if (pct < 40) return "суховато";
  if (pct < 75) return "нормально";
  if (pct < 90) return "влажно";
  return "очень мокро";
}

bool soilLooksConnected(int avgRaw, int minRaw, int maxRaw) {
  // Input pinned to a rail: open circuit with a pull-up or short to ground.
  if (avgRaw < SOIL_RAW_MIN_VALID || avgRaw > SOIL_RAW_MAX_VALID) return false;

  // The value "floats" — a characteristic sign of a high-impedance input,
  // i.e. a disconnected signal wire. A live sensor's spread
  // within a window is under 20 units (measured), the margin here is 15x.
  if (maxRaw - minRaw > SOIL_DISCONNECT_SPREAD) return false;

  return true;
}

// One window pass: N readings with a pause, power must already be applied.
// Split out separately so that on a suspicious result we can
// restart the sensor and repeat the exact same set.
static SoilWindow runWindow(int count, uint32_t gapMs) {
  SoilWindow w;
  uint32_t sum = 0;
  int mn = 4095, mx = 0;

  for (int i = 0; i < count; i++) {
    readSoil();
    sum += soilRaw;
    if (soilRaw < mn) mn = soilRaw;
    if (soilRaw > mx) mx = soilRaw;

    Serial.printf("  замер %d/%d: сырое %d\n", i + 1, count, soilRaw);

    // Exactly count-1 pauses: after the last reading there's nothing to wait for, it would only
    // delay sending. delay() feeds the watchdog; busy-waiting isn't allowed here.
    if (i < count - 1) delay(gapMs);
  }

  w.raw     = sum / count;
  w.minRaw  = mn;
  w.maxRaw  = mx;
  w.percent = soilRawToPercent(w.raw);
  // With ADC_11db the input measures up to about 3.3 V, 12-bit resolution (0..4095).
  w.voltage = w.raw * 3.3f / 4095.0f;
  w.sensorOk = soilLooksConnected(w.raw, mn, mx);
  return w;
}

SoilWindow collectWindow(int count, uint32_t gapMs) {
  if (count < 1) count = 1;

  // Power for the whole window at once, not per reading: toggling it N times
  // would mean N warm-ups of SOIL_WARMUP_MS and extra transients
  // at the ADC input — exactly what ruins reading stability.
  soilPowerOn();
  SoilWindow w = runWindow(count, gapMs);

  // Readings look suspicious — try restarting the sensor. The point isn't
  // stubbornness, but that the device sits unattended for weeks and
  // controls watering: a stuck sensor must not silently return garbage
  // on which the server would decide to water.
  for (int attempt = 1; !w.sensorOk && attempt <= SOIL_RETRY_ATTEMPTS; attempt++) {
    Serial.printf("Показания невалидны (сырое %d, разброс %d) — "
                  "перезапуск датчика, попытка %d/%d\n",
                  w.raw, w.maxRaw - w.minRaw, attempt, SOIL_RETRY_ATTEMPTS);
    soilPowerCycle();
    w = runWindow(count, gapMs);
  }

  soilPowerOff();                 // sensor de-energized until the next window

  Serial.printf("Окно из %d: сырое %d | разброс %d..%d (%d) | %.2f В | %d%% | %s%s\n",
                count, w.raw, w.minRaw, w.maxRaw, w.maxRaw - w.minRaw,
                w.voltage, w.percent, soilWord(w.percent),
                w.sensorOk ? "" : " | ДАТЧИК НЕ ОТВЕЧАЕТ");
  return w;
}

// Timer-based polling for inactive mode. A reading every soilEveryMs
// (the web page takes the fresh value from soilRaw/soilPercent); to the port
// we print one line per soilAvgCount readings — their average.
void pollSoil() {
  static unsigned long last = 0;
  static uint32_t sumRaw = 0;
  static int  count = 0;
  static int  minRaw = 0, maxRaw = 0;
  static int  prevAvg = -1;
  static int  strikes = 0;      // consecutive windows with open-circuit signs

  if (millis() - last < g_soil.soilEveryMs) return;
  last = millis();

  // Here readings are spread across loop() calls with pauses of seconds, so
  // we turn power on for each: keeping it on between readings would mean
  // powering the sensor almost constantly — exactly what we're avoiding.
  readSoilOnce();

  if (count == 0) { minRaw = maxRaw = soilRaw; }
  else {
    if (soilRaw < minRaw) minRaw = soilRaw;
    if (soilRaw > maxRaw) maxRaw = soilRaw;
  }
  sumRaw += soilRaw;
  count++;

  if (count < g_soil.soilAvgCount) return;   // window not filled yet

  int avgRaw = sumRaw / count;
  int avgPct = soilRawToPercent(avgRaw);
  float volts = avgRaw * 3.3f / 4095.0f;

  int delta = (prevAvg < 0) ? 0 : avgRaw - prevAvg;
  prevAvg = avgRaw;

  Serial.printf("[%6lus] Почва (среднее из %d) | сырое: %4d (%+4d) | разброс: %4d..%-4d | %.2f В | влажность: %3d%% | %s | пороги: сух %d / мокр %d\n",
                millis() / 1000, count, avgRaw, delta, minRaw, maxRaw,
                volts, avgPct, soilWord(avgPct), g_soil.soilDry, g_soil.soilWet);

  // Open-circuit detection: don't trigger on the first hit, so a single spike
  // (a drop, radio interference) doesn't raise a false alarm.
  if (!soilLooksConnected(avgRaw, minRaw, maxRaw)) {
    if (++strikes >= SOIL_DISCONNECT_STRIKES) {
      Serial.printf(">>> ДАТЧИК НЕ ОТВЕЧАЕТ: сырое %d, разброс %d (порог %d)\n",
                    avgRaw, maxRaw - minRaw, SOIL_DISCONNECT_SPREAD);
      strikes = 0;
    }
  } else {
    strikes = 0;
  }

  sumRaw = 0;
  count = 0;
}
