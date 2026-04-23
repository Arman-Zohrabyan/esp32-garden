#include "api.h"
#include "config.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

const char* ApiResult::cstr() const {
  switch (status) {
    case ApiStatus::Ok:         return "ok";
    case ApiStatus::NoWifi:     return "no_wifi";
    case ApiStatus::Transport:  return "transport";
    case ApiStatus::HttpError:  return "http_error";
    case ApiStatus::ParseError: return "parse_error";
  }
  return "unknown";
}

// ---------- transport ----------

// Single entry point for all requests. This is where timeouts are set and — more importantly —
// http.end() is GUARANTEED to be called. Without it, within a few dozen
// cycles the heap leaks and the board panics sometime in the night.
//
// out != nullptr -> the response body is parsed as JSON. We parse from the stream rather than
// via getString(): getString() first copies the whole body into a String, and
// peak RAM doubles.
static ApiResult doRequest(const char* method, const String& path,
                           const String& body, JsonDocument* out,
                           const JsonDocument* filter = nullptr) {
  ApiResult res;

  if (WiFi.status() != WL_CONNECTED) {
    res.status = ApiStatus::NoWifi;
    return res;
  }

  HTTPClient http;
  String url = String(API_BASE_URL) + path;

  http.begin(url);
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT);
  http.setTimeout(HTTP_IO_TIMEOUT);
  http.setReuse(false);
  if (body.length()) http.addHeader("Content-Type", "application/json");

  int code;
  if      (strcmp(method, "GET")   == 0) code = http.GET();
  else if (strcmp(method, "POST")  == 0) code = http.POST(body);
  else if (strcmp(method, "PUT")   == 0) code = http.PUT(body);
  else if (strcmp(method, "PATCH") == 0) code = http.PATCH(body);
  else                                   code = http.sendRequest(method, body);

  res.httpCode = code;

  if (code < 0) {
    Serial.printf("  %s %s -> ошибка %d (%s)\n", method, path.c_str(),
                  code, http.errorToString(code).c_str());
    res.status = ApiStatus::Transport;
    http.end();
    return res;
  }

  if (code < 200 || code >= 300) {
    Serial.printf("  %s %s -> %d\n", method, path.c_str(), code);
    res.status = ApiStatus::HttpError;
    http.end();
    return res;
  }

  if (out) {
    DeserializationError err = filter
      ? deserializeJson(*out, http.getStream(),
                        DeserializationOption::Filter(*filter))
      : deserializeJson(*out, http.getStream());
    if (err) {
      Serial.printf("  %s %s -> %d, но JSON не разобрался: %s\n",
                    method, path.c_str(), code, err.c_str());
      res.status = ApiStatus::ParseError;
      http.end();
      return res;
    }
  }

  Serial.printf("  %s %s -> %d\n", method, path.c_str(), code);
  res.status = ApiStatus::Ok;
  http.end();
  return res;
}

// ---------- sync ----------

ApiResult apiSync(DeviceConfig& dev, SoilConfig& soil, BatteryConfig& bat_cfg,
                  PumpConfig& pump, const BatteryLogPayload& bat,
                  const BatteryLogPayload& pumpBat, SyncOutcome& out) {
  out = SyncOutcome();
  out.prevDevRev  = dev.revision;
  out.prevSoilRev = soil.revision;
  out.prevPumpRev = pump.revision;

  JsonDocument body;

  JsonObject d = body["device"].to<JsonObject>();
  d["lastIp"]          = WiFi.localIP().toString();
  d["firmwareVersion"] = FW_VERSION;

  // appliedRevision is the acknowledgement "I applied revision N". Sent only
  // when something was actually applied: -1 means the board hasn't acknowledged
  // anything yet, and there's nothing to send to the server.
  if (dev.appliedRevision >= 0) d["appliedRevision"] = dev.appliedRevision;

  // The server honours isActive ONLY on first contact (unknown
  // chipId): that way a freshly created record inherits the board's local state,
  // and it doesn't "wake up disabled" after a database wipe. On a known
  // device the field is ignored, the server value always wins —
  // so false->true can't be done from here; PUT /devices/:id exists for that.
  d["isActive"] = dev.isActive;

  JsonObject s = body["soil"].to<JsonObject>();
  if (soil.appliedRevision >= 0) s["appliedRevision"] = soil.appliedRevision;

  JsonObject pm = body["pump"].to<JsonObject>();
  if (pump.appliedRevision >= 0) pm["appliedRevision"] = pump.appliedRevision;

  // Batteries ride along: there's no separate request for them. percent < 0
  // means that cell wasn't measured this cycle (or the reading is unreliable) —
  // its record isn't sent, and if neither was measured, the array isn't sent at all.
  const BatteryLogPayload* logs[] = { &bat, &pumpBat };
  for (const BatteryLogPayload* b : logs) {
    if (b->percent < 0) continue;
    JsonArray arr = body["batteryLogs"].is<JsonArray>()
                      ? body["batteryLogs"].as<JsonArray>()
                      : body["batteryLogs"].to<JsonArray>();
    JsonObject e = arr.add<JsonObject>();
    e["type"]       = b->type;
    e["volt"]       = b->volt;
    e["percentage"] = b->percent;
  }

  String payload;
  serializeJson(body, payload);

  // The filter drops createdAt/updatedAt/name/lastSeenAt and anything else the
  // firmware doesn't need: parsing dates would cost RAM on every cycle.
  JsonDocument filter;
  JsonObject fd = filter["device"].to<JsonObject>();
  fd["id"]                 = true;
  fd["revision"]           = true;
  fd["isActive"]           = true;
  fd["sleepSeconds"]       = true;
  fd["deviceLogsSeconds"]  = true;
  JsonObject fs = filter["soil"].to<JsonObject>();
  fs["soilDry"]         = true;
  fs["soilWet"]         = true;
  fs["soilEveryMs"]     = true;
  fs["soilAvgCount"]    = true;
  fs["dryAlertPercent"] = true;
  fs["autoWaterPercent"] = true;
  fs["revision"]        = true;
  JsonObject fp = filter["pump"].to<JsonObject>();
  fp["autoWaterEnabled"]      = true;
  fp["waterMl"]               = true;
  fp["flowMlPerSec"]          = true;
  fp["postWaterSleepSeconds"] = true;
  fp["maxWateringsPerDay"]    = true;
  fp["minBatteryVolt"]        = true;
  fp["maxPumpSeconds"]        = true;
  fp["revision"]              = true;
  // An array filter is given by a single sample element: it is applied
  // to every object in the array.
  JsonObject fb = filter["batteries"].add<JsonObject>();
  fb["type"]          = true;
  fb["checkEveryMs"]  = true;
  fb["checkAvgCount"] = true;
  fb["batteryAlertPercent"] = true;
  filter["batteryLogs"] = true;

  JsonDocument doc;
  ApiResult res = doRequest("POST",
                            "/devices/by-chip/" + dev.chipId + "/sync",
                            payload, &doc, &filter);
  if (!res.ok()) return res;

  JsonObject rd = doc["device"];
  if (rd.isNull()) {
    Serial.println("  sync: в ответе нет device");
    res.status = ApiStatus::ParseError;
    return res;
  }

  // id may arrive for the first time (first contact) or change if the record
  // was recreated on the server. Save it right away: without it
  // PUT /devices/:id, which activates data collection from the garden.local page, won't work.
  const char* id = rd["id"];
  if (id && strlen(id) && dev.id != id) {
    dev.id = id;
    Serial.printf("  получен id: %s\n", dev.id.c_str());
  }

  if (rd["revision"].is<int32_t>()) dev.revision = rd["revision"].as<int32_t>();

  // Fail-safe: active ONLY on an explicit boolean true. The string "false",
  // the number 1, a missing field — all treated as "inactive".
  // (rd["isActive"] | false) won't do this: there "false" would yield true.
  dev.isActive = rd["isActive"].is<bool>() && rd["isActive"].as<bool>();

  if (rd["sleepSeconds"].is<uint32_t>())
    dev.sleepSeconds = rd["sleepSeconds"].as<uint32_t>();
  if (rd["deviceLogsSeconds"].is<uint32_t>())
    dev.deviceLogsSeconds = rd["deviceLogsSeconds"].as<uint32_t>();

  // Guard against a zero schedule: sleepSeconds=0 would give an endless loop
  // of wake-ups with no pauses and drain the battery within hours.
  if (dev.sleepSeconds == 0)      dev.sleepSeconds = DEF_SLEEP_SECONDS;
  if (dev.deviceLogsSeconds == 0) dev.deviceLogsSeconds = DEF_DEVICE_LOGS_SECONDS;

  out.devChanged = (dev.revision != dev.appliedRevision);

  JsonObject rs = doc["soil"];
  if (!rs.isNull()) {
    if (rs["soilDry"].is<int32_t>())      soil.soilDry      = rs["soilDry"].as<int32_t>();
    if (rs["soilWet"].is<int32_t>())      soil.soilWet      = rs["soilWet"].as<int32_t>();
    if (rs["soilEveryMs"].is<uint32_t>()) soil.soilEveryMs  = rs["soilEveryMs"].as<uint32_t>();
    if (rs["soilAvgCount"].is<int32_t>()) soil.soilAvgCount = rs["soilAvgCount"].as<int32_t>();
    if (rs["dryAlertPercent"].is<int32_t>())
                                          soil.dryAlertPercent = rs["dryAlertPercent"].as<int32_t>();
    if (rs["autoWaterPercent"].is<int32_t>())
                                          soil.autoWaterPercent = rs["autoWaterPercent"].as<int32_t>();
    if (rs["revision"].is<int32_t>())     soil.revision     = rs["revision"].as<int32_t>();

    // Guard against a broken config: with avgCount <= 0 the reading cycle wouldn't take
    // a single measurement, with dry <= wet the percent scale would fall apart.
    if (soil.soilAvgCount < 1) soil.soilAvgCount = 1;
    if (soil.soilEveryMs == 0) soil.soilEveryMs = DEF_SOIL_EVERY_MS;
    if (soil.soilDry <= soil.soilWet) {
      Serial.printf("  ВНИМАНИЕ: калибровка невалидна (dry=%d <= wet=%d), беру дефолты\n",
                    soil.soilDry, soil.soilWet);
      soil.soilDry = DEF_SOIL_DRY;
      soil.soilWet = DEF_SOIL_WET;
    }

    out.soilChanged = (soil.revision != soil.appliedRevision);
  }

  // Pump settings. Values are applied on every response, as with
  // soil; revision only tells whether there's something to acknowledge.
  JsonObject rp = doc["pump"];
  if (!rp.isNull()) {
    if (rp["autoWaterEnabled"].is<bool>())
      pump.autoWaterEnabled = rp["autoWaterEnabled"].as<bool>();
    if (rp["waterMl"].is<int32_t>())        pump.waterMl = rp["waterMl"].as<int32_t>();
    if (rp["flowMlPerSec"].is<float>())     pump.flowMlPerSec = rp["flowMlPerSec"].as<float>();
    if (rp["postWaterSleepSeconds"].is<uint32_t>())
      pump.postWaterSleepSeconds = rp["postWaterSleepSeconds"].as<uint32_t>();
    if (rp["maxWateringsPerDay"].is<int32_t>())
      pump.maxWateringsPerDay = rp["maxWateringsPerDay"].as<int32_t>();
    if (rp["minBatteryVolt"].is<float>())   pump.minBatteryVolt = rp["minBatteryVolt"].as<float>();
    if (rp["maxPumpSeconds"].is<int32_t>()) pump.maxPumpSeconds = rp["maxPumpSeconds"].as<int32_t>();
    if (rp["revision"].is<int32_t>())       pump.revision = rp["revision"].as<int32_t>();

    // Guard against a broken config — the server validates ranges itself, this is
    // the second line. Zero flow rate would cause division by zero and
    // an endlessly running pump, zero post-watering sleep — a loop with no pauses.
    if (pump.flowMlPerSec <= 0.0f)          pump.flowMlPerSec = DEF_PUMP_FLOW;
    if (pump.waterMl < 0)                   pump.waterMl = 0;
    if (pump.postWaterSleepSeconds == 0)    pump.postWaterSleepSeconds = DEF_POST_WATER_SLEEP;
    if (pump.maxWateringsPerDay > WATERING_RING_SIZE)
                                            pump.maxWateringsPerDay = WATERING_RING_SIZE;
    if (pump.maxPumpSeconds > PUMP_ABS_MAX_SECONDS)
                                            pump.maxPumpSeconds = PUMP_ABS_MAX_SECONDS;

    out.pumpChanged  = (pump.revision != pump.appliedRevision);
    out.pumpReceived = true;
  }

  // Battery settings. The server returns an array with esp32 and pump rows:
  // esp32 — the measurement window for the board cell, pump — for the pump cell.
  JsonArray rbat = doc["batteries"];
  if (!rbat.isNull()) {
    for (JsonObject e : rbat) {
      const char* type = e["type"];
      if (type && strcmp(type, "pump") == 0) {
        if (e["checkEveryMs"].is<uint32_t>())
          pump.batCheckEveryMs = e["checkEveryMs"].as<uint32_t>();
        if (e["checkAvgCount"].is<int32_t>())
          pump.batCheckAvgCount = e["checkAvgCount"].as<int32_t>();
        if (pump.batCheckAvgCount < 1) pump.batCheckAvgCount = 1;
        if (pump.batCheckEveryMs == 0)  pump.batCheckEveryMs = DEF_BAT_EVERY_MS;
        out.pumpReceived = true;
        continue;
      }
      if (!type || strcmp(type, "esp32") != 0) continue;

      if (e["checkEveryMs"].is<uint32_t>())
        bat_cfg.checkEveryMs = e["checkEveryMs"].as<uint32_t>();
      if (e["checkAvgCount"].is<int32_t>())
        bat_cfg.checkAvgCount = e["checkAvgCount"].as<int32_t>();
      if (e["batteryAlertPercent"].is<int32_t>())
        bat_cfg.alertPercent = e["batteryAlertPercent"].as<int32_t>();

      // Same guard as for soil: a zero interval or zero number of
      // readings would make the window infinite or empty.
      if (bat_cfg.checkAvgCount < 1) bat_cfg.checkAvgCount = 1;
      if (bat_cfg.checkEveryMs == 0)  bat_cfg.checkEveryMs = DEF_BAT_EVERY_MS;

      out.batteryCfgChanged = true;
    }
  }

  JsonObject rb = doc["batteryLogs"];
  if (!rb.isNull()) {
    out.batteryAccepted = rb["accepted"] | 0;
    out.batteryRejected = rb["rejected"] | 0;
  }

  Serial.printf("  sync: devRev %d (было применено %d) | active=%s | sleep=%lus | logs=%lus\n",
                dev.revision, dev.appliedRevision, dev.isActive ? "да" : "нет",
                (unsigned long)dev.sleepSeconds,
                (unsigned long)dev.deviceLogsSeconds);
  Serial.printf("  почва: rev %d (применено %d) | dry=%d wet=%d every=%lums avg=%d\n",
                soil.revision, soil.appliedRevision, soil.soilDry, soil.soilWet,
                (unsigned long)soil.soilEveryMs, soil.soilAvgCount);
  if (out.batteryCfgChanged)
    Serial.printf("  батарея: every=%lums avg=%d alert=%d%%\n",
                  (unsigned long)bat_cfg.checkEveryMs,
                  bat_cfg.checkAvgCount, bat_cfg.alertPercent);
  if (out.pumpReceived)
    Serial.printf("  насос: rev %d (применено %d) | %s | %d мл / %.2f мл/с | порог %d%% | "
                  "сон после %lus | %d/сутки | мин %.2f В | макс %d с | банка every=%lums avg=%d\n",
                  pump.revision, pump.appliedRevision,
                  pump.autoWaterEnabled ? "вкл" : "выкл", pump.waterMl,
                  pump.flowMlPerSec, soil.autoWaterPercent,
                  (unsigned long)pump.postWaterSleepSeconds,
                  pump.maxWateringsPerDay, pump.minBatteryVolt,
                  pump.maxPumpSeconds, (unsigned long)pump.batCheckEveryMs,
                  pump.batCheckAvgCount);
  if (bat.percent >= 0 || pumpBat.percent >= 0)
    Serial.printf("  батареи отправлены: принято %d, отвергнуто %d\n",
                  out.batteryAccepted, out.batteryRejected);

  return res;
}

ApiResult apiPutIsActive(const DeviceConfig& dev, bool isActive) {
  // Without the UUID there's no way to address this route: it's the only one that goes by id
  // rather than by chipId. id comes in the sync response.
  if (!dev.id.length()) {
    ApiResult res;
    res.status = ApiStatus::HttpError;
    return res;
  }
  String payload = String("{\"isActive\":") + (isActive ? "true" : "false") + "}";
  return doRequest("PUT", "/devices/" + dev.id, payload, nullptr);
}

// ---------- report ----------

void ReportBatch::addLog(const char* level, const char* event,
                         const char* extraJson, uint32_t bootCount) {
  if (logCount >= REPORT_MAX_LOGS) {
    Serial.printf("  батч логов переполнен, отбрасываю: %s\n", event);
    return;
  }
  LogEntry& e = logEntries[logCount++];
  e.level     = level;
  e.event     = event;
  e.extra     = extraJson ? extraJson : "";
  e.bootCount = bootCount ? bootCount : rtcBootCount;
}

void ReportBatch::addReading(const SoilReadingPayload& r) {
  if (readingCount >= (int)(sizeof(readingEntries) / sizeof(readingEntries[0]))) {
    Serial.println("  батч замеров переполнен, отбрасываю замер");
    return;
  }
  readingEntries[readingCount++] = r;
}

void ReportBatch::addWatering(const WateringRecord& w) {
  if (wateringCount >= WATERING_QUEUE_SIZE) {
    Serial.println("  батч поливов переполнен, отбрасываю полив");
    return;
  }
  wateringEntries[wateringCount++] = w;
}

void ReportBatch::addWateringAfter(uint32_t nonce, int32_t moistureAfter) {
  afterNonce    = nonce;
  afterMoisture = moistureAfter;
  afterCount    = 1;
}

ApiResult apiReport(DeviceConfig& dev, ReportBatch& batch, ReportOutcome& out) {
  out = ReportOutcome();

  // The server accepts an empty body as a no-op, but there's no point running the radio for nothing.
  if (batch.empty()) {
    ApiResult res;
    res.status = ApiStatus::Ok;
    return res;
  }

  // Built by hand rather than via JsonDocument: extra arrives as a ready-made
  // fragment, and wrapping it in a document would mean parsing it and right away
  // serializing it back.
  String payload;
  payload.reserve(256 + batch.logCount * 160 + batch.readingCount * 140
                  + batch.wateringCount * 220);
  payload = "{";

  if (batch.readingCount > 0) {
    payload += "\"readings\":[";
    for (int i = 0; i < batch.readingCount; i++) {
      const SoilReadingPayload& r = batch.readingEntries[i];
      if (i) payload += ",";
      payload += "{\"raw\":";        payload += String(r.raw);
      payload += ",\"voltage\":";    payload += String(r.voltage, 2);
      payload += ",\"minRaw\":";     payload += String(r.minRaw);
      payload += ",\"maxRaw\":";     payload += String(r.maxRaw);
      payload += ",\"uptimeSec\":";  payload += String(r.uptimeSec);
      // percent and rssi are nullable on the server — if there's no data, we simply
      // don't send the field rather than sending -1, which would look like a real reading.
      if (r.percent >= 0) { payload += ",\"percent\":"; payload += String(r.percent); }
      if (r.rssi    != 0) { payload += ",\"rssi\":";    payload += String(r.rssi); }
      if (r.bootCount)    { payload += ",\"bootCount\":"; payload += String(r.bootCount); }
      payload += "}";
    }
    payload += "]";
  }

  if (batch.logCount > 0) {
    if (batch.readingCount > 0) payload += ",";
    payload += "\"logs\":[";
    for (int i = 0; i < batch.logCount; i++) {
      const ReportBatch::LogEntry& e = batch.logEntries[i];
      if (i) payload += ",";
      payload += "{\"info\":{\"chipId\":\"";
      payload += dev.chipId;
      payload += "\",\"level\":\"";
      payload += e.level;
      payload += "\",\"event\":\"";
      payload += e.event;
      payload += "\",\"uptimeSec\":";
      payload += String(millis() / 1000);
      if (e.extra.length()) {
        payload += ",";
        payload += e.extra;
      }
      payload += "},\"bootCount\":";
      payload += String(e.bootCount);
      payload += "}";
    }
    payload += "]";
  }

  // Waterings. agoSec — how many seconds ago, by the board's clock, the pump ran:
  // a watering done offline goes out cycles later, and the server timestamps it
  // "now minus agoSec" rather than with the report's arrival time.
  if (batch.wateringCount > 0) {
    if (payload.length() > 1) payload += ",";
    payload += "\"waterings\":[";
    uint32_t now = clockNow();
    for (int i = 0; i < batch.wateringCount; i++) {
      const WateringRecord& w = batch.wateringEntries[i];
      if (i) payload += ",";
      payload += "{\"nonce\":";          payload += String(w.nonce);
      payload += ",\"bootCount\":";      payload += String(w.bootCount);
      payload += ",\"ml\":";             payload += String(w.ml);
      payload += ",\"seconds\":";        payload += String(w.pumpMs / 1000.0f, 2);
      payload += ",\"flowMlPerSec\":";   payload += String(w.flowMlPerSec, 2);
      if (w.moistureBefore >= 0) { payload += ",\"moistureBefore\":"; payload += String(w.moistureBefore); }
      if (w.moistureAfter  >= 0) { payload += ",\"moistureAfter\":";  payload += String(w.moistureAfter); }
      if (w.pumpVolt > 0.0f)     { payload += ",\"pumpBatteryVolt\":"; payload += String(w.pumpVolt, 2); }
      payload += ",\"offline\":";        payload += w.offline ? "true" : "false";
      payload += ",\"agoSec\":";         payload += String(now >= w.at ? now - w.at : 0);
      payload += "}";
    }
    payload += "]";
  }

  if (batch.afterCount > 0) {
    if (payload.length() > 1) payload += ",";
    payload += "\"wateringsAfter\":[{\"nonce\":";
    payload += String(batch.afterNonce);
    payload += ",\"moistureAfter\":";
    payload += String(batch.afterMoisture);
    payload += "}]";
  }

  payload += "}";

  JsonDocument filter;
  JsonObject fr = filter["readings"].to<JsonObject>();
  fr["accepted"] = true;
  fr["rejected"] = true;
  JsonObject fl = filter["logs"].to<JsonObject>();
  fl["accepted"] = true;
  fl["rejected"] = true;
  JsonObject fw = filter["waterings"].to<JsonObject>();
  fw["accepted"] = true;
  fw["rejected"] = true;
  JsonObject fdv = filter["device"].to<JsonObject>();
  fdv["isActive"] = true;
  fdv["revision"] = true;
  filter["serverTime"] = true;

  JsonDocument doc;
  ApiResult res = doRequest("POST",
                            "/devices/by-chip/" + dev.chipId + "/report",
                            payload, &doc, &filter);
  if (!res.ok()) return res;

  out.readingsAccepted = doc["readings"]["accepted"] | 0;
  out.readingsRejected = doc["readings"]["rejected"] | 0;
  out.logsAccepted     = doc["logs"]["accepted"]     | 0;
  out.logsRejected     = doc["logs"]["rejected"]     | 0;
  out.wateringsAccepted = doc["waterings"]["accepted"] | 0;
  out.wateringsRejected = doc["waterings"]["rejected"] | 0;
  out.serverTime       = doc["serverTime"]           | 0UL;

  // Device state echo: the server may have deactivated the board while
  // readings were running (~20 s). Without this block the board would sleep for sleepSeconds in
  // a state the server has already cancelled, and would only find out on the next
  // wake-up. haveDeviceInfo distinguishes "server said false" from "server
  // didn't send the block at all" — in the latter case isActive must not be touched.
  JsonObject rdev = doc["device"];
  if (!rdev.isNull() && rdev["isActive"].is<bool>()) {
    out.haveDeviceInfo = true;
    out.isActive = rdev["isActive"].as<bool>();
    out.revision = rdev["revision"] | 0;
  }

  Serial.printf("  report: замеры %d/%d, логи %d/%d\n",
                out.readingsAccepted,
                out.readingsAccepted + out.readingsRejected,
                out.logsAccepted,
                out.logsAccepted + out.logsRejected);

  if (batch.wateringCount > 0)
    Serial.printf("  report: поливы %d/%d\n", out.wateringsAccepted,
                  out.wateringsAccepted + out.wateringsRejected);

  if (out.readingsRejected || out.logsRejected || out.wateringsRejected)
    Serial.printf("  ВНИМАНИЕ: сервер отверг записи (замеры %d, логи %d, поливы %d)\n",
                  out.readingsRejected, out.logsRejected, out.wateringsRejected);

  return res;
}

// ---------- watering ----------

ApiResult apiRequestWatering(const DeviceConfig& dev, int moisture,
                             float pumpVolt, WateringReply& out) {
  out = WateringReply();

  String payload = String("{\"moisture\":") + moisture +
                   ",\"pumpBatteryVolt\":" + String(pumpVolt, 2) + "}";

  JsonDocument filter;
  filter["allowed"]        = true;
  filter["reason"]         = true;
  filter["ml"]             = true;
  filter["flowMlPerSec"]   = true;
  filter["maxPumpSeconds"] = true;
  filter["wateringsToday"] = true;

  JsonDocument doc;
  ApiResult res = doRequest("POST",
                            "/devices/by-chip/" + dev.chipId + "/watering",
                            payload, &doc, &filter);
  if (!res.ok()) return res;

  // As with isActive: water only on an explicit boolean true.
  out.allowed = doc["allowed"].is<bool>() && doc["allowed"].as<bool>();
  const char* reason = doc["reason"];
  if (reason) strlcpy(out.reason, reason, sizeof(out.reason));
  out.ml             = doc["ml"]             | 0;
  out.flowMlPerSec   = doc["flowMlPerSec"]   | 0.0f;
  out.maxPumpSeconds = doc["maxPumpSeconds"] | 0;
  out.wateringsToday = doc["wateringsToday"] | 0;

  Serial.printf("  watering: %s%s%s | %d мл / %.2f мл/с | сегодня %d\n",
                out.allowed ? "можно" : "нельзя",
                out.reason[0] ? " — " : "", out.reason,
                out.ml, out.flowMlPerSec, out.wateringsToday);
  return res;
}
