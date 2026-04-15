#include "net.h"
#include "config.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>

String staStatus = "не подключено";
int    scanCount = -2;
bool   apOn      = true;
unsigned long lastScan = 0;

// Preferences is local to each function, not file-static: with a
// shared object a nested begin() silently keeps the PREVIOUS
// namespace (details in the comment in state.cpp).

// ---------- escaping text for insertion into HTML ----------

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&': out += F("&amp;");  break;
      case '<': out += F("&lt;");   break;
      case '>': out += F("&gt;");   break;
      case '"': out += F("&quot;"); break;
      case '\'':out += F("&#39;");  break;
      default:  out += c;
    }
  }
  return out;
}

// ---------- scanning: asynchronous, result is cached ----------

void startScan() {
  if (scanCount == -1) return;      // already in progress
  scanCount = -1;
  WiFi.scanNetworks(true);          // true = asynchronous, non-blocking
  lastScan = millis();
}

void pollScan() {
  int r = WiFi.scanComplete();
  if (r >= 0 && scanCount == -1) {
    scanCount = r;
    Serial.printf("Скан завершён: %d сетей\n", r);
  }
}

// ---------- connecting to the router ----------

bool waitConnect(const String& ssid, const String& pass, unsigned long timeoutMs) {
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(200);
  }
  return WiFi.status() == WL_CONNECTED;
}

bool connectTo(const String& ssid, const String& pass) {
  Serial.printf("Подключаюсь к «%s»...\n", ssid.c_str());

  if (waitConnect(ssid, pass, WIFI_CONNECT_TIMEOUT)) {
    staStatus = "✅ подключено — http://" + String(MDNS_HOST) +
                ".local  (IP: " + WiFi.localIP().toString() + ")";
    Serial.printf("OK! IP: %s  |  http://%s.local\n",
                  WiFi.localIP().toString().c_str(), MDNS_HOST);

    // announce the name on the network — the board can be reached at http://garden.local
    MDNS.end();
    if (MDNS.begin(MDNS_HOST)) {
      MDNS.addService("http", "tcp", 80);
      Serial.printf("mDNS активен: http://%s.local\n", MDNS_HOST);
    }
    return true;
  }

  int st = WiFi.status();
  staStatus = "❌ не вышло (код " + String(st) + ")";
  // on arduino-esp32 a wrong password usually shows up as WL_DISCONNECTED(6),
  // less often as WL_CONNECT_FAILED(4); no network — WL_NO_SSID_AVAIL(1).
  if (st == WL_DISCONNECTED || st == WL_CONNECT_FAILED)
                                   staStatus += " — неверный пароль";
  else if (st == WL_NO_SSID_AVAIL) staStatus += " — сеть не найдена";
  Serial.printf("Не вышло. Статус: %d\n", st);

  WiFi.disconnect(false);           // reset STA so it doesn't interfere with the scan
  return false;
}

// ---------- saved network ----------

bool connectSaved() {
  Preferences prefs;
  prefs.begin("wifi", true);
  String s = prefs.getString("ssid", "");
  String p = prefs.getString("pass", "");
  prefs.end();

  if (!s.length()) return false;

  Serial.printf("Есть сохранённая сеть «%s», пробую...\n", s.c_str());
  return connectTo(s, p);
}

void saveWifiCredentials(const String& ssid, const String& pass) {
  // We check the write result: this is the only way to get the board back onto the
  // network after sleep, and a silent failure here would mean "the board forgot
  // the router forever" without a single line in the logs.
  //
  // putString returns the written length (0 on error). For the empty
  // password of an open network success is indistinguishable from failure (0 == 0), but that
  // case is covered by ssid: it is written first with the same handle, and a dead NVS
  // will already fail on it.
  Preferences prefs;
  bool ok = prefs.begin("wifi", false);
  if (ok) {
    ok = prefs.putString("ssid", ssid) == ssid.length() &&
         prefs.putString("pass", pass) == pass.length();
    prefs.end();
  }

  if (ok) Serial.println("Сеть сохранена во флеш.");
  else    Serial.println("ОШИБКА: сеть НЕ сохранилась в NVS — после сна "
                         "плата её не вспомнит!");
}

void forgetWifi() {
  Preferences prefs;
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
  WiFi.disconnect(false);
  scanCount = -2;                   // the portal will start a fresh scan on visit
  staStatus = "сеть забыта — выбери новую ниже";
}

// ---------- access point ----------

void startAP() {
  WiFi.softAP(AP_SSID, AP_PASS);
  apOn = true;
}

void manageAP() {
  static unsigned long connectedSince = 0;
  const unsigned long GRACE = 60000;   // 60 s grace period so setup isn't cut off

  bool online = WiFi.status() == WL_CONNECTED;
  digitalWrite(PIN_LED, online ? HIGH : LOW);   // on = board is on the router's network

  if (online) {
    if (connectedSince == 0) connectedSince = millis();
    // enough time has passed and the AP is still on — turn it off
    if (apOn && millis() - connectedSince > GRACE) {
      WiFi.softAPdisconnect(true);
      apOn = false;
      Serial.println("AP выключен — плата в сети роутера, одно радио.");
    }
  } else {
    connectedSince = 0;
    // no connection to the router and the AP is off — bring up the fallback entry point
    if (!apOn) {
      startAP();
      Serial.println("Связь потеряна — AP поднят обратно как запасной вход.");
    }
  }
}
