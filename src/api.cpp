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
