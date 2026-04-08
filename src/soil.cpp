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
