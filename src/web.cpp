#include "web.h"
#include "config.h"
#include "state.h"
#include "soil.h"
#include "net.h"
#include "api.h"
#include "battery.h"
#include "sleepmgr.h"
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_sleep.h>

WebServer server(80);

// ---------- page: sent in chunks, without a giant String ----------

void handleRoot() {
  static int reqNum = 0;
  uint32_t before = ESP.getFreeHeap();

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html", "");

  server.sendContent(F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 Wi-Fi</title><style>"
    "body{font-family:system-ui;background:#161616;color:#eee;padding:1.5rem;"
    "max-width:480px;margin:0 auto}h2{margin:.2rem 0 1.2rem}"
    ".card{background:#242424;border-radius:12px;padding:1.2rem;margin-bottom:1rem}"
    "label{display:block;color:#888;font-size:.8rem;text-transform:uppercase;"
    "margin-bottom:.4rem;letter-spacing:.04em}"
    "select,input{width:100%;padding:.75rem;border-radius:8px;border:1px solid #3a3a3a;"
    "background:#1a1a1a;color:#eee;font-size:1rem;margin-bottom:1rem;box-sizing:border-box}"
    "button{width:100%;background:#4ade80;color:#111;border:0;padding:.9rem;"
    "border-radius:8px;font-size:1rem;font-weight:600;cursor:pointer}"
    ".st{background:#1e2a1e;border-left:3px solid #4ade80;padding:.8rem;"
    "border-radius:6px;font-size:.9rem;word-break:break-all}"
    ".lnk{display:block;text-align:center;color:#4ade80;margin-top:.8rem;font-size:.85rem}"
    ".warn{background:#2a2419;border-left-color:#fbbf24}"
    ".foot{color:#666;font-size:.75rem;text-align:center;margin-top:1rem;line-height:1.5}"
    ".row{display:flex;justify-content:space-between;align-items:baseline;"
    "padding:.55rem 0;border-bottom:1px solid #303030}"
    ".row:last-child{border-bottom:0}"
    ".row span{color:#888;font-size:.85rem}"
    ".row b{color:#4ade80;font-size:1.15rem;font-weight:600}"
    ".row i{color:#666;font-size:.75rem;font-style:normal;font-weight:400}"
    ".btn-go{display:block;text-align:center;background:#4ade80;color:#111;"
    "padding:.9rem;border-radius:8px;text-decoration:none;font-size:1rem;"
    "font-weight:600}"
    ".btn-off{display:block;text-align:center;background:#3a2020;color:#f87171;"
    "border:1px solid #5a2a2a;padding:.9rem;border-radius:8px;text-decoration:none;"
    "font-size:1rem;font-weight:600}"
    ".hint{color:#888;font-size:.8rem;text-align:center;margin-top:.8rem}"
    "</style></head><body><h2>Подключение к Wi-Fi</h2>"));

  bool online = WiFi.status() == WL_CONNECTED;

  server.sendContent("<div class='card'><label>Статус</label><div class='st'>");
  server.sendContent(staStatus);
  server.sendContent("</div></div>");

  if (online) {
    // Already connected — show the data collection controls and the disconnect button.
    // 1200, not 560: the template alone takes ~910 bytes here (Cyrillic in
    // UTF-8 is 2 bytes per character), plus substitutions. With a short buffer
    // snprintf silently truncated the page at about a third — memory stayed
    // intact, but the user saw half the dashboard.
    char act[1200];
    BatteryReading bat = batteryLast();
    snprintf(act, sizeof(act),
      "<div class='card'>"
        "<div class='row'><span>Влажность почвы</span><b>%d%% <i>%s</i></b></div>"
        "<div class='row'><span>Сырое АЦП</span><b>%d</b></div>"
        "<div class='row'><span>Батарея</span><b>%d%% <i>(%.2f В)</i></b></div>"
        "<div class='row'><span>Регистрация</span><b>%s</b></div>"
      "</div>"
      "<div class='card'>"
      "<a class='btn-go' href='/activate'>▶ Активировать устройство</a>"
      "<div class='hint'>Плата уйдёт в цикл: %d замеров с паузой %lu мс → "
      "отправка на сервер → сон %lu с. Дашборд будет недоступен во сне. "
      "Остановить — с сервера либо кнопкой BOOT при пробуждении.</div></div>"
      "<div class='card'>"
      "<a class='btn-off' href='/forget'>Отключиться от сети</a>"
      "<div class='hint'>Плата забудет сеть и вернётся в режим настройки "
      "(ESP32-Garden / 192.168.4.1)</div></div>",
      soilPercent, soilWord(soilPercent), soilRaw,
      bat.percent, bat.voltage,
      g_dev.id.length() ? "✅ на сервере" : "⏳ не зарегистрирована",
      g_soil.soilAvgCount, (unsigned long)g_soil.soilEveryMs,
      (unsigned long)g_dev.sleepSeconds);
    server.sendContent(act);
  } else {
    // Not connected — show the portal: scan + network selection + password.
    if (scanCount == -2) startScan();   // lazy scan on first visit

    server.sendContent(F("<div class='card'><form action='/save' method='POST'>"
                         "<label>Доступные сети (2.4 ГГц)</label><select name='ssid'>"));

    if (scanCount == -1) {
      server.sendContent(F("<option>⏳ сканирую... обнови через пару секунд</option>"));
    } else if (scanCount <= 0) {
      server.sendContent(F("<option>— ничего не найдено, нажми «Обновить» —</option>"));
    } else {
      char buf[200];
      for (int i = 0; i < scanCount; i++) {
        // Escape the SSID — the network name comes over the air and can break the HTML
        String ssid = htmlEscape(WiFi.SSID(i));
        snprintf(buf, sizeof(buf), "<option value='%s'>%s (%d dBm)%s</option>",
                 ssid.c_str(), ssid.c_str(), WiFi.RSSI(i),
                 WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "" : " *");
        server.sendContent(buf);
      }
    }

    server.sendContent(F("</select><label>Пароль</label>"
                         "<input type='password' name='pass' placeholder='пароль сети'>"
                         "<button type='submit'>Подключить</button></form>"
                         "<a class='lnk' href='/rescan'>🔄 Обновить список</a></div>"));
  }

  // 1100, not 640: Cyrillic in UTF-8 takes 2 bytes per character, and
  // the template with substitutions comes to ~810 bytes. With the previous size snprintf
  // truncated the page footer — the stats were shown incompletely.
  char foot[1100];
  // Firmware partition size (app0) — taken from the partition table, not from
  // getFreeSketchSpace() (which returns the size of the OTHER OTA partition).
  const esp_partition_t* run = esp_ota_get_running_partition();
  uint32_t partSz = run ? run->size : 0;           // 1280 KB with this scheme
  uint32_t sketch = ESP.getSketchSize();           // actually used by the firmware

  // uptime — how long the board has been running since the last reboot
  unsigned long sec = millis() / 1000;
  char up[24];
  if (sec < 3600) snprintf(up, sizeof(up), "%lu мин %lu с", sec / 60, sec % 60);
  else            snprintf(up, sizeof(up), "%lu ч %lu мин", sec / 3600, (sec % 3600) / 60);

  // Wi-Fi signal level and its verbal rating (online is declared above)
  int rssi = online ? WiFi.RSSI() : 0;
  const char* q = !online ? "нет сети" :
                  rssi >= -55 ? "отличный" :
                  rssi >= -67 ? "хороший"  :
                  rssi >= -75 ? "средний"  : "слабый";

  // temperatureRead() on this core version returns degrees Fahrenheit
  float tempF     = temperatureRead();
  float tempF_to_C = (tempF - 32.0f) / 1.8f;

  snprintf(foot, sizeof(foot),
    "<div class='card'>"
      "<div class='row'><span>RAM</span><b>%u КБ <i>свободно из %u</i></b></div>"
      "<div class='row'><span>DISC</span><b>%u КБ <i>свободно из %u</i></b></div>"
      "<div class='row'><span>Аптайм</span><b>%s</b></div>"
      "<div class='row'><span>Сигнал Wi-Fi</span><b>%d dBm <i>%s</i></b></div>"
      "<div class='row'><span>Температура <i>(макс 85°)</i></span>"
      "<b>%.1f °C <i>= %.0f °F</i></b></div>"
    "</div>"
    "<div class='foot'>RAM — оперативка, меняется в реальном времени · "
    "DISC — флеш: прошивка занимает %u КБ · "
    "сигнал: ближе к 0 — лучше · "
    "температура кристалла грубая и неточная</div>"
    "</body></html>",
    ESP.getFreeHeap() / 1024, ESP.getHeapSize() / 1024,
    (partSz - sketch) / 1024, partSz / 1024,
    up, rssi, q,
    tempF_to_C, tempF,
    sketch / 1024);
  server.sendContent(foot);
  server.sendContent("");

  uint32_t after = ESP.getFreeHeap();
  Serial.printf("Запрос #%d | heap до: %u после: %u дельта: %+d | мин: %u\n",
                ++reqNum, before, after, (int)after - (int)before,
                ESP.getMinFreeHeap());
}

void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");

  server.send(200, "text/html", F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta http-equiv='refresh' content='20;url=/'>"
    "<style>body{font-family:system-ui;background:#161616;color:#eee;"
    "padding:2rem;text-align:center}</style></head><body>"
    "<h3>Подключаюсь...</h3><p>Страница обновится сама через 20 секунд.</p>"
    "<p style='color:#888;font-size:.85rem'>Сеть ESP32-Garden продолжает "
    "работать — если пароль неверный, просто попробуй снова.</p></body></html>"));

  if (connectTo(ssid, pass)) saveWifiCredentials(ssid, pass);
  startScan();                      // refresh the list after the attempt
}


// ---------- routes ----------

void webBegin() {
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/rescan", []() {
    WiFi.scanDelete();
    scanCount = -2;
    startScan();
    server.sendHeader("Location", "/");
    server.send(303);
  });
  server.on("/forget", []() {
    forgetWifi();
    server.sendHeader("Location", "/");
    server.send(303);
  });
  // Device activation. The only path for isActive false -> true:
  // by protocol only the device itself can turn itself on, and only
  // locally — after confirmation by a human on this page.
  server.on("/activate", []() {
    // A UUID is required: PUT /devices/:id is the only route that goes by
    // id rather than chipId. The id arrives in the sync response, so an empty field
    // means the board has never once reached the server.
    if (!g_dev.id.length()) {
      server.send(200, "text/html", F(
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='5;url=/'>"
        "<style>body{font-family:system-ui;background:#161616;color:#eee;"
        "padding:2rem;text-align:center}b{color:#f87171}</style></head><body>"
        "<h3><b>Нет связи с сервером</b></h3>"
        "<p style='color:#888;font-size:.85rem'>Плата ещё не синхронизировалась — "
        "сначала нужно подключиться к сети роутера.</p>"
        "</body></html>"));
      return;
    }

    // ORDER MATTERS: first the server, then NVS, then sleep.
    // If we save locally and go to sleep but the server never learns about it, it
    // will never send isActive=false — and the board could then be woken
    // only with the BOOT button.
    //
    // Specifically PUT, not sync: for an already known device the server's
    // isActive always overrides what the board sends in the sync body,
    // so it can't turn itself on via sync — only via this route.
    ApiResult res = apiPutIsActive(g_dev, true);

    if (!res.ok()) {
      char page[700];
      snprintf(page, sizeof(page),
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta http-equiv='refresh' content='5;url=/'>"
        "<style>body{font-family:system-ui;background:#161616;color:#eee;"
        "padding:2rem;text-align:center}b{color:#f87171}</style></head><body>"
        "<h3><b>Не удалось активировать</b></h3>"
        "<p style='color:#888;font-size:.85rem'>Сервер не ответил (%s, код %d). "
        "Плата осталась в обычном режиме — попробуй ещё раз.</p>"
        "</body></html>", res.cstr(), res.httpCode);
      server.send(200, "text/html", page);
      Serial.printf(">>> Активация НЕ прошла: %s (%d)\n", res.cstr(), res.httpCode);
      return;
    }

    // The server accepted — now it's safe to commit locally.
    saveIsActive(true);

    char page[900];
    snprintf(page, sizeof(page),
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<style>body{font-family:system-ui;background:#161616;color:#eee;"
      "padding:2rem;text-align:center}b{color:#4ade80}</style></head><body>"
      "<h3>▶ Устройство активировано</h3>"
      "<p>Цикл: <b>%d замеров</b> с паузой <b>%lu мс</b> → отправка → "
      "сон <b>%lu с</b>.</p>"
      "<p style='color:#888;font-size:.85rem'>Дашборд недоступен во сне. "
      "Остановить: выключить с сервера либо держать кнопку <b>BOOT</b> "
      "на плате, пока она просыпается.</p></body></html>",
      g_soil.soilAvgCount, (unsigned long)g_soil.soilEveryMs,
      (unsigned long)g_dev.sleepSeconds);
    server.send(200, "text/html", page);

    delay(300);                       // let the page reach the browser
    Serial.println("\n>>> Устройство активировано. Ухожу в deep sleep.");
    goToSleep(g_dev.sleepSeconds);    // doesn't return; powers down the sensor and voltage divider
  });
  server.onNotFound([]() { server.send(404, "text/plain", ""); });
  server.begin();
}

void webLoop() {
  server.handleClient();
}
