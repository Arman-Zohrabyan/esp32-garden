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
