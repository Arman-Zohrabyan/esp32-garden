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
