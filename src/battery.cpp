#include "battery.h"
#include "config.h"
#include "state.h"

// Li-ion discharge curve: voltage -> percent.
//
// The linear formula ((V-3.0)/1.2*100) gives a badly wrong result here.
// The reason is the curve's shape: the 3.7-3.9 V plateau holds about 60% of capacity, and
// a linear conversion would show 68% where it's actually 50%. The user
// would see "half charge" for hours, then a sudden zero — because
// the last 10% drops from 3.5 to 3.0 V within just minutes.
//
// Points go from charged to discharged; we interpolate between them.
struct CurvePoint { float volts; int percent; };

static const CurvePoint CURVE[] = {
  {4.20f, 100}, {4.06f, 90}, {3.98f, 80}, {3.92f, 70},
  {3.87f,  60}, {3.82f, 50}, {3.79f, 40}, {3.75f, 30},
  {3.68f,  20}, {3.50f, 10}, {3.00f,  0},
};
static const int CURVE_LEN = sizeof(CURVE) / sizeof(CURVE[0]);

// Voltage divider ratio: (R1+R2)/R2. With two 100k resistors it's 2.0.
static const float DIVIDER = (BATTERY_R1 + BATTERY_R2) / BATTERY_R2;

static BatteryReading lastReading;
static float smoothedPercent = -1.0f;   // -1 = smoothing not started yet

static bool     dividerOn   = false;
static uint32_t dividerOnAt = 0;        // millis() when the voltage divider was enabled

void batteryBegin() {
  // 11 dB — full input range (~0..3.1 V). With a 1:2 voltage divider and a
  // 4.2 V battery the pin sees 2.1 V, which fits with margin.
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BATTERY, ADC_11db);

  // Enable the voltage divider (GPIO26 -> LOW) and keep it on until sleep: while the board is awake,
  // its ~2 µA is lost against tens of mA. goToSleep() turns it off via
  // batteryDividerOff(). A repeated call doesn't restart the charging timer.
  if (!dividerOn) {
    pinMode(PIN_BAT_EN, OUTPUT);
    digitalWrite(PIN_BAT_EN, LOW);
    dividerOn   = true;
    dividerOnAt = millis();
  }
}

void batteryDividerOff() {
  pinMode(PIN_BAT_EN, INPUT);           // no pulls: circuit is open
  dividerOn = false;
}

float batteryVoltage() {
  // The voltage divider midpoint charges in ~5 ms (τ) after enabling; we wait 10τ.
  // Only wait the first time after batteryBegin(): after that it's already charged.
  uint32_t since = millis() - dividerOnAt;
  if (dividerOn && since < BATTERY_SETTLE_MS) delay(BATTERY_SETTLE_MS - since);

  // analogReadMilliVolts, NOT analogRead: the former uses the factory
  // ADC calibration from eFuse. The ESP32's raw ADC is nonlinear, with error
  // reaching 6% at the edges of the range.
  uint32_t sum = 0;
  for (int i = 0; i < BATTERY_SAMPLES; i++) {
    sum += analogReadMilliVolts(PIN_BATTERY);
    delayMicroseconds(200);
  }
  int mv = sum / BATTERY_SAMPLES;
  lastReading.mvOnPin = mv;

  return (mv * DIVIDER * BATTERY_CALIB) / 1000.0f;
}

int batteryPercent(float volts) {
  if (volts >= CURVE[0].volts)             return 100;
  if (volts <= CURVE[CURVE_LEN - 1].volts) return 0;

  for (int i = 0; i < CURVE_LEN - 1; i++) {
    float vHi = CURVE[i].volts,     vLo = CURVE[i + 1].volts;
    int   pHi = CURVE[i].percent,   pLo = CURVE[i + 1].percent;
    if (volts <= vHi && volts >= vLo) {
      float t = (volts - vLo) / (vHi - vLo);
      return (int)lroundf(pLo + t * (pHi - pLo));
    }
  }
  return 0;
}

BatteryReading batteryRead() {
  float v = batteryVoltage();
  int   p = batteryPercent(v);

  // Exponential smoothing: without it the percent flickers back and forth by one
  // from ADC noise, which is quite annoying on the dashboard.
  if (smoothedPercent < 0) smoothedPercent = p;
  else smoothedPercent = smoothedPercent * (1.0f - BATTERY_SMOOTH) + p * BATTERY_SMOOTH;

  lastReading.voltage = v;
  lastReading.percent = (int)lroundf(smoothedPercent);
  return lastReading;
}

BatteryReading batteryLast() {
  return lastReading;
}

BatteryReading batteryWindow(int count, uint32_t gapMs) {
  if (count < 1) count = 1;

  // A window of readings with pauses — same as the soil sensor. A single reading catches
  // the instantaneous state, and under load Li-ion voltage "breathes":
  // it sags on a current pulse and recovers within seconds.
  // Averaging over the window gives the resting voltage, not a random point.
  float sum = 0.0f;
  int   mvSum = 0;

  for (int i = 0; i < count; i++) {
    sum   += batteryVoltage();
    mvSum += lastReading.mvOnPin;

    // No pause after the last reading: it would just delay the cycle
    // for nothing. Exactly count-1 pauses, same as in the soil window.
    if (i < count - 1) delay(gapMs);
  }

  float v = sum / count;
  int   p = batteryPercent(v);

  if (smoothedPercent < 0) smoothedPercent = p;
  else smoothedPercent = smoothedPercent * (1.0f - BATTERY_SMOOTH) + p * BATTERY_SMOOTH;

  lastReading.voltage = v;
  lastReading.percent = (int)lroundf(smoothedPercent);
  lastReading.mvOnPin = mvSum / count;
  return lastReading;
}

// Pump cell voltage divider ratio: R3/R4 at 1 MΩ each, same as the main one.
static const float PUMP_DIVIDER = (PUMP_BAT_R1 + PUMP_BAT_R2) / PUMP_BAT_R2;

BatteryReading pumpBatteryWindow(int count, uint32_t gapMs) {
  if (count < 1) count = 1;

  // ADC resolution is already set in batteryBegin(); attenuation is set per
  // pin. 11 dB: with a 1:2 voltage divider a full cell gives 2.1 V at the pin.
  analogSetPinAttenuation(PIN_PUMP_BAT, ADC_11db);

  // Enable the voltage divider only for the duration of the reading. While the bottom of R4 was floating, the midpoint
  // was pulled up to the cell voltage through R3; after enabling, it settles to
  // half through 0.5 MΩ on 10 nF (τ = 5 ms), and 50 ms is 10τ.
  pinMode(PIN_PUMP_BAT_EN, OUTPUT);
  digitalWrite(PIN_PUMP_BAT_EN, LOW);
  delay(BATTERY_SETTLE_MS);

  uint32_t mvSum = 0;
  for (int i = 0; i < count; i++) {
    uint32_t sum = 0;
    for (int k = 0; k < BATTERY_SAMPLES; k++) {
      sum += analogReadMilliVolts(PIN_PUMP_BAT);
      delayMicroseconds(200);
    }
    mvSum += sum / BATTERY_SAMPLES;
    if (i < count - 1) delay(gapMs);
  }

  pinMode(PIN_PUMP_BAT_EN, INPUT);        // no pulls: voltage divider is open

  BatteryReading r;
  r.mvOnPin = mvSum / count;
  r.voltage = (r.mvOnPin * PUMP_DIVIDER * PUMP_BAT_CALIB) / 1000.0f;
  r.percent = batteryPercent(r.voltage);
  return r;
}

void pollBattery() {
  static unsigned long last = 0;

  // Its own interval from the battery settings (checkEveryMs), not soilEveryMs:
  // these are different values with different meanings, and they used to be linked by
  // oversight — which made the battery log spam at the soil reading rate.
  if (millis() - last < g_bat.checkEveryMs) return;
  last = millis();

  // In inactive mode we don't collect a window: loop() must not block for
  // seconds, or the web server stalls. A single reading with smoothing
  // is enough here — the page is refreshed manually, accuracy isn't critical.
  BatteryReading b = batteryRead();

  // A verbal assessment by thresholds — so a problem is visible in the log right away,
  // not just a number.
  const char* state = b.voltage <= BATTERY_CRITICAL_VOLTS ? "КРИТИЧНО"
                    : b.voltage <= BATTERY_LOW_VOLTS      ? "РАЗРЯЖЕНА"
                    : b.voltage <= BATTERY_WARN_VOLTS     ? "низкий заряд"
                    : "норма";

  Serial.printf("[BAT] %d%% | %.2f В | на пине %d мВ | %s\n",
                b.percent, b.voltage, b.mvOnPin, state);
}
