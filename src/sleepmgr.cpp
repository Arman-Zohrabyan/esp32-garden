#include "sleepmgr.h"
#include "config.h"
#include "state.h"
#include "battery.h"
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <driver/rtc_io.h>

String chipId() {
  // We use esp_read_mac(ESP_MAC_WIFI_STA), NOT ESP.getEfuseMac():
  // the latter returns 48 bits in reversed byte order, and the string
  // would come out backwards relative to what
  // esptool shows and what the server expects.
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);

  char buf[13];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

const char* resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";   // normal wake-up
    case ESP_RST_PANIC:     return "PANIC";       // firmware crash
    case ESP_RST_BROWNOUT:  return "BROWNOUT";    // power sag
    case ESP_RST_INT_WDT:                         // all three watchdogs —
    case ESP_RST_TASK_WDT:                        // same symptom:
    case ESP_RST_WDT:       return "WDT";         // hang
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";         // EN button
    case ESP_RST_SW:        return "SW";          // esp_restart()
    default:                return "OTHER";
  }
}

bool bootReasonIsFailure() {
  switch (esp_reset_reason()) {
    case ESP_RST_BROWNOUT:   // not enough power — most often on the radio
    case ESP_RST_PANIC:      // firmware crash
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:        // hang, watchdog fired
      return true;
    default:
      return false;
  }
}

bool bootButtonPressed() {
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  delay(50);                        // debounce
  return digitalRead(PIN_BOOT_BTN) == LOW;
}

// ---------- active cycle indication ----------

static hw_timer_t* blinkTimer = nullptr;

// Timer interrupt. IRAM_ATTR is mandatory: the handler must live in
// fast memory, otherwise during flash access (which happens constantly —
// NVS, reading constants) the interrupt will land at a moment when the flash is busy, and
// the board will panic.
//
// Only the pin toggle inside: no Serial, malloc or driver
// calls — all of that is forbidden in an interrupt.
static void IRAM_ATTR onBlinkTimer() {
  digitalWrite(PIN_LED, !digitalRead(PIN_LED));
}

void blinkStart() {
  if (blinkTimer) return;                  // already running

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  // Timer 0, prescaler 80: 80 MHz clock / 80 = 1 MHz, i.e. one
  // tick is a microsecond. Counting in microseconds is convenient and clear.
  blinkTimer = timerBegin(0, 80, true);
  timerAttachInterrupt(blinkTimer, &onBlinkTimer, true);

  // Toggle the state every half second — giving a one-second period:
  // 0.5 s on, 0.5 s off.
  timerAlarmWrite(blinkTimer, 500000, true);
  timerAlarmEnable(blinkTimer);
}

void blinkStop() {
  if (!blinkTimer) return;

  timerAlarmDisable(blinkTimer);
  timerDetachInterrupt(blinkTimer);
  timerEnd(blinkTimer);
  blinkTimer = nullptr;

  digitalWrite(PIN_LED, LOW);
}

// ---------- pins during sleep ----------
//
// GPIO25 (sensor power) and GPIO26 (voltage divider switch) must be
// high-impedance during sleep: no pulls and not an output. Any pull on
// GPIO26 would close the voltage divider and draw current from the cell all sleep, and an output on
// GPIO25 would keep the sensor powered. rtc_gpio_isolate removes the pulls
// and holds this state until the next wake-up.
//
// Same for the pump unit (firmware 0.4.0): GPIO27 is isolated, and the IRLZ44N
// switch is held off by its 10 kΩ pull resistor; GPIO33 is isolated, and
// the pump cell voltage divider is open.

static void sleepPinsIsolate() {
  digitalWrite(PIN_SOIL_POWER, LOW);        // sensor de-energized
  pinMode(PIN_SOIL_POWER, INPUT);
  batteryDividerOff();                      // GPIO26 -> input, voltage divider open
  digitalWrite(PIN_PUMP, LOW);              // pump off
  pinMode(PIN_PUMP, INPUT);
  pinMode(PIN_PUMP_BAT_EN, INPUT);          // pump cell voltage divider open
  rtc_gpio_isolate((gpio_num_t)PIN_SOIL_POWER);
  rtc_gpio_isolate((gpio_num_t)PIN_BAT_EN);
  rtc_gpio_isolate((gpio_num_t)PIN_PUMP);
  rtc_gpio_isolate((gpio_num_t)PIN_PUMP_BAT_EN);
}

void sleepPinsRelease() {
  // The hold survives deep sleep: without this, after wake-up
  // pinMode/digitalWrite on GPIO25 and GPIO26 silently have no effect — the sensor
  // won't turn on, the voltage divider won't close. On power-on there is no hold,
  // and the call is harmless.
  rtc_gpio_hold_dis((gpio_num_t)PIN_SOIL_POWER);
  rtc_gpio_hold_dis((gpio_num_t)PIN_BAT_EN);
  rtc_gpio_hold_dis((gpio_num_t)PIN_PUMP);
  rtc_gpio_hold_dis((gpio_num_t)PIN_PUMP_BAT_EN);

  // Pump — output LOW immediately, before any other work: the pull on the
  // gate holds the switch off even without this, but an explicit LOW leaves the pin no chance
  // to pick up interference while the board boots.
  pinMode(PIN_PUMP, OUTPUT);
  digitalWrite(PIN_PUMP, LOW);
}

void goToSleep(uint32_t seconds) {
  blinkStop();                      // remove the interrupt before entering sleep
  digitalWrite(PIN_LED, LOW);       // turn off the LED before sleep
  sleepPinsIsolate();               // sensor and voltage divider de-energized for the entire sleep
  Serial.printf("Засыпаю на %lu с (держи BOOT при пробуждении для стопа)\n",
                (unsigned long)seconds);
  Serial.flush();                   // let the log reach the port before shutdown

  // Board clock (see clockNow): this cycle's awake time plus sleep. Right
  // before sleep — after this millis() won't add anything.
  rtcClockSec += millis() / 1000 + seconds;

  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();           // we don't return here — the board shuts down
}
