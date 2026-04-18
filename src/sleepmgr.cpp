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
