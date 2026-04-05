#pragma once

// All hardware settings and initial values in one place.
//
// Important note on the source of truth: the DEF_* values are DEFAULTS, not
// operating constants. The real values come from the server (Device and SoilSettings) and
// are cached in NVS. DEF_* are used only when the server has never responded
// even once — i.e. on first boot or with an empty cache.

// ---------- identification and network ----------

// FW_VERSION is set in platformio.ini via build_flags. The duplicate here is
// in case of a build without the flags (e.g. straight from the IDE).
#ifndef FW_VERSION
#define FW_VERSION "0.4.0"
#endif

// We use http, not https. Rationale: mbedtls already takes 81 KB in the
// firmware regardless of use (HTTPClient pulls in WiFiClientSecure
// unconditionally), so TLS wouldn't cost flash — but it would cost 1-3 seconds
// of handshake and ~40-50 KB of peak heap per request. When running on
// battery that is noticeable. The price: the config travels over the network in plain text.
#define API_BASE_URL "http://esp32.novacoteam-server.com"

// Client-side HTTP timeouts. The server field pingTimeoutSeconds is not
// related: it is the server's policy for "when to consider the board offline"; the firmware
// neither uses nor stores it.
#define HTTP_CONNECT_TIMEOUT 4000   // ms to establish TCP
#define HTTP_IO_TIMEOUT      6000   // ms for the exchange

#define WIFI_CONNECT_TIMEOUT       15000  // ms, normal mode
#define WIFI_CONNECT_TIMEOUT_SLEEP 10000  // ms, active cycle — shorter

// ---------- Device defaults (match the server's) ----------

#define DEF_SLEEP_SECONDS        50
#define DEF_DEVICE_LOGS_SECONDS  10

// ---------- SoilSettings defaults ----------

// Capacitive sensor calibration. The scale is inverted: dry gives a HIGHER
// voltage, wet a LOWER one, hence DRY > WET. The values were measured on
// a specific sensor unit; another one will have its own.
#define DEF_SOIL_DRY 3245   // in air / dry soil       -> 0 %
#define DEF_SOIL_WET 1090   // in a glass of water     -> 100 %

// How many readings we average and with what pause between them. They come from
// the server and mean literally this: until all soilAvgCount
// readings are taken, the board doesn't send data and doesn't go to sleep.
#define DEF_SOIL_EVERY_MS   2000
#define DEF_SOIL_AVG_COUNT  10

// "Dry" alert threshold. The firmware does NOT act on it — it is needed by
// server-side analytics and the dashboard. The board only transmits percent.
#define DEF_DRY_ALERT_PERCENT 30

// Auto-watering threshold: a reading at or below this percent turns on the pump (see
// watering.h). Separate from DEF_DRY_ALERT_PERCENT — the alert and watering
// can trigger at different moisture levels.
#define DEF_AUTO_WATER_PERCENT 20

// ---------- Battery defaults (row type="esp32") ----------
//
// They come from the server in the batteries[] block of the sync response and are cached in NVS.
// Same meaning as for soil: how many readings we average and with what pause.
#define DEF_BAT_EVERY_MS      2000
#define DEF_BAT_AVG_COUNT     10
#define DEF_BAT_ALERT_PERCENT 30

// ---------- disconnected sensor detection ----------

// MEASURED on a live board: with the sensor CONNECTED and in air, the spread
// (maxRaw-minRaw) within a window is under 20 units, the mean ~3240..3289.
// An open circuit was NOT measured — the thresholds below are conservative and need calibration:
// pull the sensor connector and watch the log for about 30 seconds.
//
// The main criterion is the spread, not the absolute value: with an
// open circuit the ADC input is high-impedance and picks up noise, the value "floats".
// A 15x margin over normal makes a false trigger unlikely.
#define SOIL_DISCONNECT_SPREAD 300

// Second scenario: the input is pinned to a rail (short circuit, or open circuit with a pull-up).
// The spread is then near zero instead, so we catch it by absolute value.
#define SOIL_RAW_MIN_VALID 200
#define SOIL_RAW_MAX_VALID 4000

// Consecutive windows before triggering — protection against a single outlier (a drop,
// noise from the Wi-Fi transmitter, which runs nearby while sending).
#define SOIL_DISCONNECT_STRIKES 2

// Pause when restarting the sensor: cut power, wait, read again.
// One second is not enough: there's an electrolytic cap on the sensor's LDO output, and with the input
// unpowered the voltage holds for hundreds of milliseconds more. If it isn't given time to
// discharge, the sensor won't reset its internal state and will return the same
// stuck value — the restart will be illusory.
#define SOIL_RESET_MS 2500

// How many restart attempts before declaring the sensor dead.
#define SOIL_RETRY_ATTEMPTS 2

// ---------- pins and hardware ----------

#define PIN_LED 2        // blue LED: on = board is on the router's network

// GPIO32 = ADC1 input. Specifically ADC1, not ADC2 (GPIO 0/2/4/12-15/25-27):
// ADC2 is physically used by the radio and can't be read while Wi-Fi is on.
#define PIN_SOIL 32

// The sensor is powered from GPIO25 (D25 on the board), not from the 3V3 pin.
// Rationale: a capacitive sensor under constant voltage in wet soil
// corrodes — electrolysis destroys the electrodes within months. Via GPIO
// power is applied only for the duration of a reading: seconds per hour instead of 24/7.
//
// Limitation: a GPIO safely sources ~20 mA (40 mA max). The capacitive
// sensor v1.2 draws ~5 mA — a fourfold margin. If you fit a
// more power-hungry sensor, you'll need a transistor as a switch.
#define PIN_SOIL_POWER 25

// How long to wait after applying power before reading the ADC.
// The capacitive sensor needs time to settle: the oscillator has to
// start up, the output filter has to charge. A reading right after power-on
// gives an underestimated value. 50 ms is generous; if readings turn out
// unstable in the first samples, increase it.
#define SOIL_WARMUP_MS 50

#define PIN_BOOT_BTN 0   // BOOT button: hold on wake-up = emergency stop

// ---------- battery reading ----------

// GPIO34 — ADC1 input, input-only. ADC2 (GPIO 0/2/4/12-15/25-27) won't do:
// it shares resources with the radio and can't be read while Wi-Fi is on.
#define PIN_BATTERY 34

// Board power: cell -> TP4056 -> J1 -> XC6220 (3.3 V) -> 3V3 pin.
// The cell, voltage divider and ESP32 share a common ground, so the reading is valid even with
// USB connected. (In the previous circuit with the MT3608, on USB it showed half
// the value — that problem went away along with the MT3608.)

// Voltage divider: R1 and R2 are 1 MOhm each, midpoint on GPIO34, 10 nF between
// the midpoint and ground. Required because the ADC measures up to 3.3 V,
// while Li-ion gives up to 4.2 V.
//
// The bottom of R2 goes not to ground but to GPIO26 — this is the voltage divider's switch.
// GPIO26 LOW: divider on, GPIO34 sees half the cell. GPIO26 as an input
// without pulls (also during sleep): circuit open, zero current through the divider. So
// the resistances can be large — current during a reading is ~2 uA — and the
// 10 nF keeps the midpoint steady while the ADC "bites off" charge for a sample.
//
// Jumper J2 on the board disconnects the divider from the cell entirely. Then
// GPIO34 sees almost 0 V, and the reading is rejected by BATTERY_VALID_MIN_VOLTS.
#define BATTERY_R1 1000000.0f
#define BATTERY_R2 1000000.0f

// GPIO26 = voltage divider switch (see above). An ADC2 pin, but here it is only
// a digital output, and Wi-Fi doesn't interfere with ADC2 pins' digital functions.
#define PIN_BAT_EN 26

// How long to wait after turning on the divider before the first reading. The midpoint
// charges through 0.5 MOhm (R1 || R2) into 10 nF: τ = 5 ms, so 50 ms is 10τ.
#define BATTERY_SETTLE_MS 50

// Calibration factor. Compensates both for resistor tolerance (typical ones are
// 5%, two 100k ones can give a divider ratio of 1.9 instead of 2.0) and residual
// ADC error.
//
// How to tune: measure the actual battery voltage with a multimeter,
// see what the log shows, and compute
//   BATTERY_CALIB = actual / shown
// Usually it comes out at 1.02-1.08. One point is enough.
#define BATTERY_CALIB 1.0f

// How many samples we average per reading.
#define BATTERY_SAMPLES 32

// Exponential smoothing factor for the percentage: the smaller, the
// smoother, but the slower the response to a real change.
#define BATTERY_SMOOTH 0.1f

// ---------- charge thresholds ----------
//
// The software cutoff doesn't replace the TP4056 board's protection, it complements it:
// the hardware one only trips at 2.5 V, when the cell has already spent several hours
// in the accelerated-aging zone. The point of the software one is not to get there.

// Warning: send a notification, increase intervals.
#define BATTERY_WARN_VOLTS 3.6f

// Below this — don't turn on Wi-Fi. At transmitter peak (up to 400 mA) the cell, wires
// and XC6220 lose another 0.1-0.2 V, and the ESP32 will brown out. The active cycle
// in this case immediately sleeps for BATTERY_LOW_SLEEP_SECONDS (see main.cpp).
// Don't turn on the pump either: the motor's inrush current will drag the cell down the same way.
#define BATTERY_LOW_VOLTS 3.45f

// Hysteresis: after a cutoff at BATTERY_LOW_VOLTS, Wi-Fi turns back on
// only above this voltage, i.e. after charging. Without it, a cell
// that recovered to 3.46 V during sleep would turn on Wi-Fi, sag and go back to
// sleep — and so on in a loop, finishing itself off.
#define BATTERY_RESUME_VOLTS 3.6f

// Sleep with a depleted cell: 12 hours. Sleep current is tens of uA, so it
// doesn't finish off the cell, and the board will notice charging within 12 hours at most.
#define BATTERY_LOW_SLEEP_SECONDS (12UL * 3600)

// Only for the log label. Below BATTERY_LOW_VOLTS the board already sleeps for
// 12 hours without Wi-Fi.
#define BATTERY_CRITICAL_VOLTS 3.3f

// Below this voltage the reading is considered invalid and isn't sent to the server.
// A live Li-ion cell never gives that little: the TP4056 protection cuts off the
// load already at 2.5 V. So such a value means not discharge but a
// removed jumper J2 or a disconnected voltage divider. Sending it to battery_logs
// is not allowed: the chart would fill up with zeros indistinguishable from a real discharge.
// Such a reading doesn't trigger the BATTERY_LOW_VOLTS protection either.
#define BATTERY_VALID_MIN_VOLTS 2.5f

// Above this — also invalid: a full cell gives 4.2 V. More happens
// only if the divider didn't turn on (GPIO26 not LOW) — then the bottom of R2
// floats, GPIO34 receives almost the whole cell, and the ADC hits its ceiling.
#define BATTERY_VALID_MAX_VOLTS 4.5f

// ---------- pump (auto-watering, since firmware 0.4.0) ----------
//
// The pump is powered from a SEPARATE cell (its own TP4056); the ESP32 only opens
// the switch. Circuit: pump cell + -> MT3608 (5 V) -> pump; the MT3608 ground goes
// to the IRLZ44N drain, source to common ground. Switch closed — the boost converter
// is fully unpowered; the pump cell supplies nothing in sleep or while
// awake, except during watering itself.

// GPIO27 -> 100 Ohm -> IRLZ44N gate, 10 kOhm between gate and ground.
// HIGH = pump running. While the pin isn't configured (reset, boot, sleep), the
// 10 kOhm pull-down keeps it off. An ADC2 pin, but here only a digital
// output — Wi-Fi doesn't interfere with it.
#define PIN_PUMP 27

// The pump cell's voltage divider is a copy of the main one: R3/R4 1 MOhm each, 10 nF from
// the midpoint to ground, jumper J5 disconnects it from the cell entirely.
// Midpoint on GPIO35 (ADC1_CH7, input-only), bottom of R4 on GPIO33.
//
// Unlike the main divider, this one is turned on only for the duration of a
// reading (GPIO33 LOW) and immediately turned off: the board doesn't touch the pump cell
// more than necessary. In sleep GPIO33 is isolated, zero current through the divider.
#define PIN_PUMP_BAT    35
#define PIN_PUMP_BAT_EN 33
#define PUMP_BAT_R1 1000000.0f
#define PUMP_BAT_R2 1000000.0f

// Calibration — same as BATTERY_CALIB: actual (multimeter) / shown.
#define PUMP_BAT_CALIB 1.0f

// PumpSettings defaults — match the server's. Used until the server has
// sent its own, and for watering without connectivity if the NVS cache is empty.
#define DEF_PUMP_ENABLED         true
#define DEF_WATER_ML             350     // ml per watering
#define DEF_PUMP_FLOW            26.67f  // ml/s at 5 V: 1000 ml in 37.5 s (measured)
#define DEF_POST_WATER_SLEEP     600     // s, sleep after watering until the check reading
#define DEF_MAX_WATERINGS_DAY    3       // waterings in any 24 hours
#define DEF_PUMP_MIN_VOLT        3.3f    // below this — no watering
#define DEF_MAX_PUMP_SECONDS     60      // limit for one run

// Absolute ceiling for one run, whatever the server sends. The server
// checks its own limits itself; this is the second line: a pump left
// running will empty the tank onto the floor.
#define PUMP_ABS_MAX_SECONDS 300

// How many recent waterings the board remembers for the daily limit without connectivity.
// Must be no less than the server maximum maxWateringsPerDay (8).
#define WATERING_RING_SIZE 8

// How many waterings can wait to be sent to the server (RTC memory). One watering
// per cycle, 10-minute sleep after watering — four is enough for several hours
// without connectivity; on overflow the oldest one is evicted.
#define WATERING_QUEUE_SIZE 4

// Default sleep duration until the server has sent its sleepSeconds.
// In step 9 this will be replaced with g_dev.sleepSeconds.
#define SLEEP_US (10ULL * 1000000)

#define AP_SSID   "ESP32-Garden"
#define AP_PASS   "12345678"
#define MDNS_HOST "garden"       // board address: http://garden.local

// ---------- reliability limits ----------

// Connectivity failure counter. Previously the board switched to web mode based on it after
// five cycles; now the decision is made by the readings buffer (readings.h) — it
// gives up only when it is full. The counter remains as diagnostics: its
// value goes into the wifi_failed log as the missedCycles field, and it shows
// how many wake-ups passed without connectivity.
#define MAX_FAIL_STREAK 5

// Rated endurance of an NVS sector — erase cycles. The number is from the datasheet for
// the ESP32 flash memory; used only to estimate wear in logs, it doesn't affect
// firmware behavior.
//
// The real margin is higher: NVS spreads writes across the sector (wear leveling),
// so "write counter / endurance" is an UPPER estimate. If it
// shows 10%, the real wear is lower.
#define NVS_ENDURANCE 100000UL
