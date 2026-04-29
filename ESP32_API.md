# ESP32 Backend API Documentation

This document describes the backend API for ESP32 firmware/client developers. It covers all HTTP endpoints, request/response shapes, and the database schema behind them.

> **Status note:** This backend is a minimal NestJS + PostgreSQL (TypeORM) service. There is currently **no authentication** on any endpoint, **no request validation** (any JSON is accepted and passed straight to the database), and **no MQTT/WebSocket support** — all communication is plain HTTP + JSON.

---

## 1. Connection basics

| | |
|---|---|
| Protocol | HTTP (REST, JSON) |
| Base URL | `http://<server-host>:<PORT>` (default port `3055`) |
| Global prefix | none — routes are at the root, e.g. `http://host:3055/devices` |
| Content-Type | `application/json` for all POST/PUT/PATCH bodies |
| Auth | **None.** No API key, token, or header is required or checked. |
| CORS | Disabled (not relevant to ESP32 firmware, relevant if a web dashboard is added later) |

---

## 2. Core concept: revision-based config sync

Two resources (`Device` and `SoilSettings`) each carry their own **`revision`** (int, server-owned) that increments any time a config-relevant field is changed by a human/API caller. The device stores the last revision it successfully applied in its own memory/flash and reports it back as **`appliedRevision`** on its next check-in — that's the "confirm."

Unlike an earlier version of this API, the device does **not** send its known revision to decide the response shape and does **not** call separate confirm endpoints. A single call, `POST /devices/by-chip/:chipId/sync` (see [3.1](#31-devices--devices)), always returns the full `Device` and `SoilSettings` objects and accepts both `appliedRevision` values in the same request — the device just diffs the returned `revision` against what it already applied, and includes the new `appliedRevision` on its *next* `sync` call once it's done applying. This collapses what used to be up to 4 separate round-trips (register/ping, soil-settings GET, two confirms) into one.

This pattern is used for:
- **Device-level config** (`sleepSeconds`, `isActive`)
- **Soil sensor calibration** (`SoilSettings`)

---

## 3. Endpoints

### 3.1 Devices — `/devices`

Device identity is keyed by two different fields depending on the route:
- **`chipId`** (string, unique, set by firmware — the ESP32 doesn't have a true "chip ID", so this should be the station MAC address with no separators, e.g. `8C94DF8B7EF8` for MAC `8C:94:DF:8B:7E:F8` — 12 hex characters, not 6). This is the identifier the board always has, even before it has ever talked to the server (it comes from eFuse and survives `erase_flash`), so it's what the firmware-facing `sync`/`report` endpoints are addressed by.
- **`id`** (UUID, server-generated) — used for the dashboard-facing `PUT` route. The device also gets its `id` back in every `sync` response and may store it, but nothing the firmware calls requires it as a precondition.

#### `GET /devices`
List all devices, newest first.

**Response 200** — `Device[]`

#### `GET /devices/:chipId`
Look up a device by its hardware chip ID. Read-only — does **not** create the device (use `sync`, below, for that).

**Path params:** `chipId` (string)

**Response 200** — `Device`
**Response 404** — if no device has that `chipId` yet

#### `POST /devices`
Explicitly create/pre-configure a device via JSON body (e.g. from an admin UI), with full control over initial config values. Optional — not required for a board to self-register, since `sync` (below) creates the device automatically on first contact.

**Body** (`Partial<Device>` — typically):
```json
{
  "chipId": "8C94DF8B7EF8",
  "name": "Balcony Plant",
  "firmwareVersion": "1.0.0",
  "sleepSeconds": 50,
  "isActive": true
}
```

**Response 201** — created `Device` (includes generated `id`, `revision: 1`, timestamps). Also provisions the device's default `SoilSettings` row and its two `Battery` rows (`esp32` + `pump`) — see [3.5](#35-battery-tracking--battery-battery-logs-battery-normalization).

#### `PUT /devices/:id`
Dashboard-facing update. If the body includes `sleepSeconds` and/or **`isActive`**, the server **bumps `revision`** automatically so the device picks up the change on its next `sync`. `lastSeenAt` is **not** touched — it tracks actual device contact (`sync`/`report`), and a dashboard edit is not the device checking in.

**Path params:** `id` (UUID)

**Body** — any subset of `Device` fields, e.g.:
```json
{ "sleepSeconds": 120, "name": "Renamed device" }
```

**Response 200** — updated `Device`
**Response 404** — if `id` not found

---

##### `isActive` — ownership and behavior

`isActive` has **asymmetric ownership depending on direction**:

- **`false → true`**: **local side only, and only at first contact.** A brand-new board reports its own `isActive` in the `device` body of its first `sync` call, and the server takes it as-is since it has no prior opinion (see `sync` below). Once a device row exists, `sync` no longer reads `device.isActive` from the body **at all** — not to activate, not to deactivate — the server's stored value is simply left untouched by `sync` from that point on. Re-activating an existing device requires a dashboard/operator call to `PUT /devices/:id` with `{ "isActive": true }`.
- **`true → false`**: **`PUT /devices/:id` only.** The backend/operator deactivates a device remotely by calling `PUT /devices/:id` with `{ "isActive": false }`. This is also how a device would deactivate itself locally (e.g. via the **BOOT button**) — the firmware would issue this same `PUT` call, not report it through `sync`, since `sync` ignores `isActive` on every call after the first.

In short: only the device can turn itself *on*, and only during its very first sync; `isActive` can only be turned *off* — by anyone, device or operator — via `PUT /devices/:id`. `sync` is a one-way door for this field: it can seed the initial value, never change it afterward.

> **No caller distinction.** `PUT /devices/:id` has no way to know whether the request came from the device itself (e.g. a local BOOT-button handler) or from an operator/dashboard — there's no authentication on any endpoint (see [Known gaps](#6-known-gaps-worth-knowing-before-relying-on-this-api)). The "device-only for on, either side for off" rule above is a protocol convention firmware and backend must both respect; the server does not and cannot enforce who is allowed to call `PUT` with which value.

**What `isActive` controls on the device:**

- **`isActive === false`:** the device must **not** run its normal sleep cycle or take sensor measurements. Instead, it should stay in a lightweight loop that calls `sync` and then submits only a heartbeat log (no `readings`) via `report` every **`deviceLogsSeconds`** (default `300`) seconds. `sleepSeconds`/soil sampling are effectively suspended.
- **`isActive === true`:** the device runs its normal cycle — deep sleep for `sleepSeconds`, take soil readings, submit them, etc. `deviceLogsSeconds` is **not used** in this mode.

**Wake-up check (important — fail-safe default):** on every wake from deep sleep, the firmware should re-check `isActive` from the server (via `sync`, since `isActive` changes bump `revision`) before resuming its normal cycle. Treat **anything other than `isActive === true`** (i.e. `false`, missing, or malformed) as inactive — do not assume "active" as the default. In other words: the device only proceeds with sleep/measurement behavior when the server has explicitly confirmed `isActive === true`; every other case falls back to the heartbeat-only loop. Because `report`'s response also echoes back the current `isActive`/`revision` (see [`POST .../report`](#post-devicesby-chipchipidreport)), a deactivation that happens mid-cycle is caught before the device sleeps, not just on the following wake.

#### `POST /devices/by-chip/:chipId/sync`
The single call the ESP32 makes at the start of every wake cycle. Replaces what used to be self-registration + heartbeat/`ping` + the soil-settings `GET` + both revision-confirm `PATCH`es + battery-log submission + battery-config `GET`.

**Path params:** `chipId` (string)

**Body:**
```json
{
  "device": {
    "lastIp": "192.168.1.42",
    "appliedRevision": 3,
    "isActive": true
  },
  "soil": {
    "appliedRevision": 2
  },
  "pump": {
    "appliedRevision": 1
  },
  "batteryLogs": [
    { "type": "esp32", "volt": 3.7, "percentage": 82 },
    { "type": "pump", "volt": 3.95, "percentage": 74 }
  ]
}
```
| Field | Type | Required | Notes |
|---|---|---|---|
| `device.lastIp` | string | no | device's current local IP, stored on the device record |
| `device.firmwareVersion` | string | no | updates the stored value if provided |
| `device.appliedRevision` | number | no | confirms the device-config revision the board has applied |
| `device.isActive` | boolean | no | the board's own current state — **only used when this `chipId` is unknown to the server** (see below); ignored on every later call |
| `device.name` | string | no | only applied when this is a new device |
| `soil.appliedRevision` | number | no | confirms the soil-settings revision the board has applied |
| `pump.appliedRevision` | number | no | confirms the pump-settings revision the board has applied (firmware 0.4.0+) |
| `batteryLogs` | array | no | zero or more `{ type, volt, percentage }` entries, one per battery being reported this cycle — see [Battery tracking](#35-battery-tracking--battery-battery-logs-battery-normalization) |
| `batteryLogs[].type` | `"esp32"` \| `"pump"` | yes (per entry) | which of the device's two `Battery` rows this reading belongs to — resolved via `(deviceChipId, type)`, both rows always exist once the device does (see below) |
| `batteryLogs[].volt` | number | yes (per entry) | measured voltage |
| `batteryLogs[].percentage` | integer | yes (per entry) | battery percentage; the server does no computation, so this is whatever the firmware derives (ideally via [`battery_normalization`](#35-battery-tracking--battery-battery-logs-battery-normalization)) |

Firmware 0.4.0+ measures both: the ESP32's own supply (`type: "esp32"`, divider midpoint on **GPIO34**, low side switched by **GPIO26**) and the pump's separate cell (`type: "pump"`, divider midpoint on **GPIO35**, low side switched by **GPIO33**). A reading the firmware judges invalid (below 2.5 V or above 4.5 V — a removed jumper J2/J5, a disconnected cell) is left out of the array. Sending `batteryLogs` is entirely optional; omit it (or send an empty array) on cycles where nothing new was measured.

**Behavior:**
- **Unknown `chipId`:** the server creates the device (`isActive` taken from `device.isActive`, defaulting to `false` if omitted), plus its default `SoilSettings`, `PumpSettings` and `Battery` (`esp32` + `pump`) rows — exactly what `POST /devices` provisions. This is what used to be a distinct "register" call; a wiped database and a genuinely new board now hit the identical code path, so firmware doesn't need a separate registration branch or 404-triggered recovery logic.
- **Known `chipId`:** the server's own `isActive` always wins over `device.isActive` in the body (see [`isActive` — ownership and behavior](#isactive--ownership-and-behavior)). `lastSeenAt` is updated on every call; `lastIp`/`firmwareVersion` are updated if provided; either `appliedRevision`, if provided, is applied as a confirm.
- Each `batteryLogs[]` entry is validated independently — an unknown `type` or a missing `volt`/`percentage` rejects just that entry (reported back in `batteryLogs.errors`, same shape as [`report`](#report--devicesby-chipchipidreport)'s per-entry errors), it doesn't fail the whole `sync` call.
- The response **always** contains the full `device`, `soil` and `pump` objects — there's no revision-diffing/minimal-response mode. A device that predates pump settings gets its `PumpSettings` row (defaults) on its first `sync`. The firmware compares the returned `revision` against what it last applied and reacts accordingly, then includes the new `appliedRevision` on a later `sync` call once it's done applying.
- The response also always contains `batteries` — one entry per `Battery` row belonging to this device (`esp32` + `pump`), with that battery's `checkEveryMs`/`checkAvgCount`. This is how the firmware finds out how often/how many samples to average per battery check — there's no separate battery-config fetch, and no revision tracking on this (unlike `device`/`soil`, battery config isn't a sync'd resource — the firmware just reads the latest value every cycle).

**Response 200/201** (201 on first-contact creation, 200 otherwise):
```json
{
  "device": {
    "id": "…uuid…",
    "chipId": "8C94DF8B7EF8",
    "name": "Balcony Plant",
    "lastSeenAt": "2026-08-15T12:00:00.000Z",
    "lastIp": "192.168.1.42",
    "firmwareVersion": "1.0.0",
    "sleepSeconds": 120,
    "isActive": true,
    "deviceLogsSeconds": 300,
    "revision": 4,
    "appliedRevision": 3,
    "createdAt": "…",
    "updatedAt": "…"
  },
  "soil": {
    "id": "…uuid…",
    "deviceId": "…uuid…",
    "soilDry": 3050,
    "soilWet": 900,
    "soilEveryMs": 2000,
    "soilAvgCount": 10,
    "dryAlertPercent": 30,
    "autoWaterPercent": 20,
    "revision": 2,
    "appliedRevision": 2,
    "updatedAt": "…"
  },
  "pump": {
    "id": "…uuid…",
    "deviceId": "…uuid…",
    "autoWaterEnabled": true,
    "waterMl": 350,
    "flowMlPerSec": 26.67,
    "postWaterSleepSeconds": 600,
    "maxWateringsPerDay": 3,
    "minBatteryVolt": 3.3,
    "maxPumpSeconds": 60,
    "revision": 1,
    "appliedRevision": 1,
    "updatedAt": "…"
  },
  "batteries": [
    { "type": "esp32", "checkEveryMs": 500, "checkAvgCount": 10 },
    { "type": "pump", "checkEveryMs": 500, "checkAvgCount": 10 }
  ],
  "batteryLogs": { "accepted": 2, "rejected": 0 }
}
```
| Field | Notes |
|---|---|
| `pump` | the device's `PumpSettings` — see [3.6](#36-auto-watering--pump-settings-waterings) |
| `batteries[].type` | `"esp32"` or `"pump"` |
| `batteries[].checkEveryMs` | intended interval between battery voltage checks for this battery |
| `batteries[].checkAvgCount` | intended number of raw samples to average per battery reading |

---

#### `POST /devices/by-chip/:chipId/report`
The single call the ESP32 makes at the end of a wake cycle (or whenever it has data ready) to submit sensor readings and/or diagnostic logs. Replaces `POST /devices/:deviceId/soil-readings` + `POST /device-logs`. Also refreshes the device's `lastSeenAt`, same as `sync` — a `report` is a device contact in its own right.

**Path params:** `chipId` (string)

**Body:**
```json
{
  "readings": [
    { "raw": 2650, "percent": 42, "voltage": 2.31, "minRaw": 1090, "maxRaw": 3245, "rssi": -62, "uptimeSec": 184, "bootCount": 47 }
  ],
  "logs": [
    { "info": { "chipId": "8C94DF8B7EF8", "level": "info", "event": "cycle_done", "freeHeap": 214560 }, "bootCount": 47 }
  ],
  "waterings": [
    { "nonce": 2804211931, "bootCount": 47, "ml": 350, "seconds": 13.12, "flowMlPerSec": 26.67, "moistureBefore": 18, "pumpBatteryVolt": 3.95, "offline": false, "agoSec": 2 }
  ],
  "wateringsAfter": [
    { "nonce": 1931557205, "moistureAfter": 46 }
  ]
}
```
| Field | Type | Required | Notes |
|---|---|---|---|
| `readings` | array | no | zero or more `SoilReading` objects — see field details in [3.3](#33-soil-sensor-readings--devicesdeviceidsoil-readings) |
| `readings[].bootCount` | integer | no | see [`bootCount`](#bootcount--wake-correlation) below |
| `logs` | array | no | zero or more `{ info, bootCount? }` objects — `info` shape as documented in [3.4](#34-device-logs--device-logs) |
| `logs[].bootCount` | integer | no | see [`bootCount`](#bootcount--wake-correlation) below |
| `waterings` | array | no | pump runs (firmware 0.4.0+), one per watering, sent from the board's RTC queue until accepted |
| `waterings[].nonce` | integer (uint32) | yes | random id the board gives each watering. A resend with a known nonce counts as accepted but is not inserted again — so a report whose reply got lost can't create duplicates |
| `waterings[].ml` / `.seconds` | number | yes | dose asked for, and how long the pump actually ran |
| `waterings[].flowMlPerSec`, `.moistureBefore`, `.moistureAfter`, `.pumpBatteryVolt`, `.bootCount` | number | no | as stored in [`watering_logs`](#49-watering_logs) |
| `waterings[].offline` | boolean | no | `true` = watered on cached settings, without the server's answer |
| `waterings[].agoSec` | integer | no | seconds between the pump run and this report, by the board's own clock (counts through deep sleep). The server stores `wateredAt = now − agoSec`, so a watering done offline lands at the time it actually happened |
| `wateringsAfter` | array | no | `{ nonce, moistureAfter }` — the moisture measured on the wake after the post-watering sleep, for a watering already reported earlier. Unknown nonce → rejected |

**Both fields are independently optional, and both are arrays, not single objects** — real firmware cycles hit all four combinations of "with/without a reading" × "with/without logs": a sensor-disconnected cycle sends only a `sensor_disconnected` log and no reading; a normal cycle sends one reading plus a `cycle_done` log; a cycle recovering from a failed upload can resend a previous cycle's buffered reading (from RTC memory) alongside the current one, meaning `readings` can have more than one entry; a post-`BROWNOUT` cycle can carry several queued logs (`boot`, `wifi_failed`, `cycle_done`) at once. An entirely empty body (`{}`, or `{"readings":[],"logs":[]}`) is accepted as a no-op — the firmware doesn't need to special-case "nothing to send."

Each array entry is validated independently: a `readings[]` entry missing any required field (`raw`, `voltage`, `minRaw`, `maxRaw`, `uptimeSec`), or a `logs[]` entry missing `info`, is rejected on its own without failing the rest of the batch.

**Response 201:**
```json
{
  "readings": { "accepted": 1, "rejected": 0 },
  "logs": { "accepted": 1, "rejected": 0 },
  "waterings": { "accepted": 1, "rejected": 0 },
  "wateringsAfter": { "accepted": 1, "rejected": 0 },
  "device": { "isActive": true, "revision": 4 },
  "serverTime": 1755230400
}
```
| Field | Notes |
|---|---|
| `readings.accepted` / `readings.rejected` | counts, not ids — the firmware doesn't read individual reading ids back, so they aren't sent |
| `readings.errors` | present only if `rejected > 0`: `[{ index, reason }]`, index into the `readings` array that was sent |
| `logs.accepted` / `logs.rejected` / `logs.errors` | same shape, for `logs` |
| `waterings.*` / `wateringsAfter.*` | same shape, for `waterings` / `wateringsAfter` |
| `device.isActive` / `device.revision` | current server-side state, echoed back so a mid-cycle deactivation or config change is caught **before** the device sleeps, rather than only being noticed on the next wake's `sync` |
| `serverTime` | Unix seconds — the board has no RTC/clock of its own, so this lets it timestamp a *deferred* reading (one buffered in RTC memory from a failed upload) with something closer to the truth than "whenever it finally got resent" |

**Do not discard a retry buffer unless `rejected === 0` for that array.** The firmware should only clear its RTC-buffered pending reading/logs once the corresponding `accepted`/`rejected` counts confirm the server actually persisted everything it was sent — checking only the HTTP status code is not enough to know a partial batch was fully accepted.

##### `bootCount` — wake correlation

`bootCount` is the device's own monotonic wake counter (kept in RTC memory, survives deep sleep, reset only by a power loss or `erase_flash`). It's optional and nullable — omit it and existing behavior is unaffected — but worth sending because it enables a few things a timestamp alone can't:

- **Detecting missed cycles.** A gap in the sequence on the server (`41, 42, 47`) means cycles 43–46 never made it — the board was offline or crash-looping — which is not distinguishable from a raised `sleepSeconds` by timestamp alone.
- **Correctly dating a deferred/resent reading.** A reading resent a cycle late (see `report` above) arrives with a `recordedAt` timestamp from the resend, not the original measurement — its `bootCount` should be the wake it was actually **taken** on, not the wake it was **uploaded** on, so it doesn't land in the wrong place on a time-series chart.
- **Tying a `BROWNOUT` boot log to the cycle that actually failed.** The `boot` log documenting a brownout is necessarily sent from the *next* successful wake, so without `bootCount` the correlation to the failed cycle is lost.

Values are always `≥ 1` (the counter starts at 1 on RTC init) and effectively never wrap (even waking every 50s, it takes ~3,500 years to reach the `int` ceiling). Not usable as an idempotency key on its own — a resend after a lost response will legitimately reuse the same `bootCount` — so it's indexed as `(deviceId, bootCount)`, unique only in combination with per-device scoping, not globally.

#### `POST /devices/by-chip/:chipId/watering`
Asked by the board (firmware 0.4.0+) right before it runs the pump: "soil is at X %, pump cell at Y V — may I, and how much?". Writes nothing — the watering itself arrives later in `report.waterings[]`, with how long the pump actually ran. Refreshes `lastSeenAt`.

**Body:** `{ "moisture": 18, "pumpBatteryVolt": 3.95 }` (both optional)

**Response 201:**
```json
{ "allowed": true, "reason": null, "ml": 350, "flowMlPerSec": 26.67, "seconds": 13.12, "maxPumpSeconds": 60, "wateringsToday": 0, "maxWateringsPerDay": 3 }
```
`reason` (when `allowed` is `false`), checked in this order: `disabled` (`autoWaterEnabled` off), `daily_limit` (`watering_logs` rows with `wateredAt` in the last 24 h ≥ `maxWateringsPerDay`), `pump_battery_low` (`pumpBatteryVolt < minBatteryVolt`), `not_dry` (`moisture > autoWaterPercent`). The board computes the run time itself as `ml / flowMlPerSec`, capped by `maxPumpSeconds` (and by a 300 s firmware ceiling); `seconds` is the same calculation, returned for reference. **404** for an unknown `chipId`.

---

### 3.2 Soil sensor settings — `/devices/:deviceId/soil-settings`

Per-device calibration/config for the soil moisture sensor. `:deviceId` is the device's UUID `id`.

> **Dashboard-facing only.** The device reads and confirms soil settings via [`sync`](#post-devicesby-chipchipidsync) instead — these two routes exist for the admin dashboard (`device.html`) to view/edit calibration values by hand.
>
> **Hardware note:** the sensor's analog output (`AOUT`) is read on **GPIO32** (`PIN_SOIL` in `config.h`), and its `VCC` is switched via **GPIO25** (`PIN_SOIL_POWER` in `config.h`) — the sensor is power-cycled per reading rather than left continuously powered, to reduce electrolytic corrosion of the probe over time.

#### `GET /devices/:deviceId/soil-settings`
Fetch current settings for a device.

**Response 200** — `SoilSettings`
**Response 404** — if no settings row exists yet for this device

#### `PUT /devices/:deviceId/soil-settings`
Create or update (upsert) settings for a device. Bumps `revision` on every update; sets `revision: 1` on first creation.

**Body** (`Partial<SoilSettings>`):
```json
{
  "soilDry": 3050,
  "soilWet": 900,
  "soilEveryMs": 2000,
  "soilAvgCount": 10,
  "dryAlertPercent": 30,
  "autoWaterPercent": 20
}
```

**Response 200** — the resulting `SoilSettings`

> `autoWaterPercent` is the auto-watering trigger: when a measurement comes out at or below it, the board runs the pump (see [3.6](#36-auto-watering--pump-settings-waterings)). It is part of soil settings, so changing it bumps `revision` like any other field here.

> `soilAvgCount` directly determines how each `SoilReading` is produced: the device takes `soilAvgCount` raw ADC samples (spaced out internally) and submits their **average** as a single `SoilReading` — see [3.3](#33-soil-sensor-readings--devicesdeviceidsoil-readings) below. A `SoilReading` is never a single raw sample; it's always the average of the last `soilAvgCount` samples at submission time.

---

### 3.3 Soil sensor readings — `/devices/:deviceId/soil-readings`

Time-series telemetry submitted by the device after each sensor sample.

> **Each reading is an average, not a single raw sample.** Per the device's `soilAvgCount` setting (see [3.2](#32-soil-sensor-settings--devicesdeviceidsoil-settings)), the firmware takes that many raw ADC readings, averages them on-device, and submits the result as one `SoilReading` — `raw`, `percent`, and `voltage` are all derived from that averaged value, sampled every `soilEveryMs` (also from `SoilSettings`). The server does no averaging of its own; it stores whatever the device sends.

> **Dashboard-facing only.** The device submits readings via [`report`](#post-devicesby-chipchipidreport) instead — this route is for the admin dashboard to list them.

#### `GET /devices/:deviceId/soil-readings`
List recent readings for a device, newest first.

**Query params:** `limit` (optional, integer, default `100`)

**Response 200** — `SoilReading[]`

| Field | Type | Required (in `report`) | Notes |
|---|---|---|---|
| `raw` | int | yes | raw ADC reading, averaged on-device over `soilAvgCount` samples |
| `percent` | smallint | no | moisture percentage; omit if the computation failed/was skipped |
| `voltage` | real | yes | sensor voltage, also from the averaged sample |
| `minRaw` | int | yes | calibration min at time of reading |
| `maxRaw` | int | yes | calibration max at time of reading |
| `rssi` | smallint | no | WiFi signal strength (dBm); omit if unreadable |
| `uptimeSec` | int | yes | device uptime in seconds at time of reading |
| `bootCount` | int | no | see [`bootCount` — wake correlation](#bootcount--wake-correlation) |

---

### 3.4 Device logs — `/device-logs`

Logging endpoint, scoped to a device via `deviceId` (the device's UUID `id`). `info` is a freeform jsonb blob — technically anything goes — but it should be reserved for **what can't be reconstructed after the fact**. Regular telemetry belongs in `soil-readings`; `device-logs` is for events, not measurements.

This is also the heartbeat channel used while a device is **inactive** (`isActive !== true`) — see [isActive — ownership and behavior](#isactive--ownership-and-behavior). In that mode, the device should submit a log entry via `report` every `deviceLogsSeconds` seconds instead of running its normal sleep/measurement cycle, so the server still sees it as alive.

> **Dashboard-facing only.** The device submits logs via [`report`](#post-devicesby-chipchipidreport) instead — this route is for the admin dashboard to list them.

#### `GET /device-logs`
List all logs, newest first.

**Response 200** — `DeviceLog[]`

Log entry shape, as submitted in the `logs` array of `report`:

```json
{
  "info": {
    "chipId": "8C94DF8B7EF8",
    "level": "error",
    "event": "wifi_failed",
    "ssid": "TEAM_COSMO-7E36",
    "status": 6,
    "waitedMs": 10000
  },
  "bootCount": 47
}
```
| Field | Type | Required | Notes |
|---|---|---|---|
| `info` | object | yes | freeform jsonb, see conventions and event catalog below |
| `bootCount` | int | no | see [`bootCount` — wake correlation](#bootcount--wake-correlation) |

`info` is stored as-is (jsonb) — any JSON object shape is accepted, but see below for the recommended shape. `deviceId` is not part of the payload — the server resolves it from the `:chipId` in the `report` URL.

---

##### Format conventions

- **Keep a common envelope on every entry:** `chipId`, `level`, `event`, `uptimeSec`. A schemaless jsonb column turns into an unqueryable dump fast without at least this much consistency.
- **`event` is a machine identifier, not prose.** Use `"wifi_failed"`, not `"WiFi reconnect failed"` — the former is filterable/groupable in a query, the latter isn't.
- **`level`** — one of `"info"`, `"warn"`, `"error"`.

##### What to log — by purpose

**Heartbeat** (while `isActive === false`, every `deviceLogsSeconds`):
```json
{
  "info": {
    "chipId": "8C94DF8B7EF8",
    "level": "info",
    "event": "heartbeat",
    "uptimeSec": 184,
    "freeHeap": 214560,
    "rssi": -52,
    "ip": "192.168.11.82",
    "isActive": false,
    "nvsWrites": 0,
    "nvsWearPct": 0
  }
}
```
`nvsWrites`/`nvsWearPct` — see [NVS wear tracking](#nvs-wear-tracking) below.

**Errors — the most important category.** This is the entire reason the log endpoint exists: without it, a device that fails silently and goes back to sleep leaves no trace of why data stopped arriving.

- Wi-Fi connect failure:
  ```json
  {"info": {"chipId": "…", "level": "error", "event": "wifi_failed",
    "ssid": "TEAM_COSMO-7E36", "status": 6, "waitedMs": 10000}}
  ```
- Ping/HTTP failure:
  ```json
  {"info": {"chipId": "…", "level": "error", "event": "ping_failed",
    "httpCode": -1, "error": "connection refused", "url": "…"}}
  ```
- Sensor disconnected (e.g. raw reading pinned at the ADC ceiling with no spread):
  ```json
  {"info": {"chipId": "…", "level": "error", "event": "sensor_disconnected",
    "raw": 4095, "minRaw": 1200, "maxRaw": 4095, "spread": 2895}}
  ```

**Boot / reset diagnostics.** The single most-overlooked category, and usually the one that explains the strangest bugs. `esp_reset_reason()` tells you *why* the board restarted:

| `esp_reset_reason_t` | Meaning |
|---|---|
| `ESP_RST_DEEPSLEEP` | normal wake from deep sleep — expected |
| `ESP_RST_PANIC` | firmware crash |
| `ESP_RST_BROWNOUT` | power sag — undersized supply/battery |
| `ESP_RST_WDT` | hang, watchdog fired |

```json
{"info": {"chipId": "…", "level": "warn", "event": "boot",
  "resetReason": "BROWNOUT", "bootCount": 47, "firmwareVersion": "1.2.0"}}
```
A `BROWNOUT` on a battery-powered device is a direct signal of insufficient power — without this log, all you'd observe is "the board restarts sometimes."

**Power and memory** (battery-powered devices — log every active cycle):
```json
{"info": {"chipId": "…", "event": "cycle_done",
  "freeHeap": 214560, "minFreeHeap": 198004,
  "batteryMv": 3720, "awakeMs": 4200,
  "nvsWrites": 0, "nvsWearPct": 0}}
```
`awakeMs` is the key battery-life metric — how long the device stayed awake this cycle. A creeping value (e.g. Wi-Fi taking longer to associate over time) is an early warning the battery will drain faster than expected. `minFreeHeap` catches slow memory leaks. `nvsWrites`/`nvsWearPct` — see [NVS wear tracking](#nvs-wear-tracking) below.

**Low-battery sleep** (firmware 0.3.0+). Below 3.45 V the active cycle does not turn Wi-Fi on — the transmitter's current peaks would sag the cell into a brownout — and sleeps 12 h instead, until the cell is back above 3.6 V (i.e. charged). Those wakes can't report anything, so once the device is back online it sends one log with how many such sleeps happened:
```json
{"info": {"chipId": "…", "level": "warn", "event": "battery_low_sleep",
  "sleeps": 3, "sleepSeconds": 43200, "lowVolts": 3.45, "resumeVolts": 3.60}}
```

**Config applied** — confirms a revision was actually applied on-device, not just written to the DB:
```json
{"info": {"chipId": "…", "event": "config_applied",
  "revision": 7, "prevRevision": 6,
  "changed": {"sleepSeconds": [50, 600], "isActive": [false, true]}}}
```

##### NVS wear tracking

`nvsWrites`/`nvsWearPct` (present on `heartbeat` and `cycle_done`) track cumulative writes to NVS flash over the board's lifetime, since flash cells have a finite erase/write endurance — repeatedly writing a buffered-readings block (e.g. the retry queue used to survive a failed upload, see [`bootCount` — wake correlation](#bootcount--wake-correlation)) is the main thing that would consume it.

| Field | Type | Notes |
|---|---|---|
| `nvsWrites` | int | cumulative NVS write count over the board's lifetime |
| `nvsWearPct` | int | `nvsWrites / endurance × 100`, rounded — endurance is a fixed constant (`100000` writes), not per-board measured |

`0` is the expected steady-state value while the device stays within a normal cycle: the retry buffer only gets written when an upload actually fails, so a healthy board accumulates writes rarely, not every cycle.

In addition to the running counts on every `heartbeat`/`cycle_done`, a dedicated **`nvs_wear`** event fires once when cumulative wear crosses the 50/80/90/95% thresholds — not on every cycle past that point, to avoid spamming the log once a board is in its wear-out phase:

```json
{"info": {"chipId": "…", "level": "error", "event": "nvs_wear",
  "nvsWrites": 80000, "nvsWearPct": 80, "threshold": 80,
  "endurance": 100000,
  "note": "upper-bound estimate, wear leveling not accounted for"}}
```
`level` is `"warn"` below 80% and `"error"` at 80% and above. Each threshold fires only once per board lifetime — firmware tracks which thresholds have already been reported in RTC memory (`rtcWearReported`), not flash, since persisting that state to NVS would itself consume the write endurance being tracked. A `bootCount`-only power loss (not `erase_flash`) resets RTC state, so a threshold could in principle re-fire after a battery pull even though the underlying wear didn't reverse — this is a known tradeoff of not persisting the reported-set to flash.

---

### 3.5 Battery tracking — `/battery`, `/battery-logs`, `/battery-normalization`

A **battery** represents a physical battery pack that powers either an ESP32 board or a pump, identified by `type`. Every battery is tied to the device it powers via `deviceChipId` (FK to `devices.chipId`).

> **Provisioning is automatic; config and logging are live; calibration is manual.** Two `Battery` rows — one `esp32`, one `pump` — are created automatically the moment a `Device` is created, whether via [`sync`](#post-devicesby-chipchipidsync)'s first-contact path or `POST /devices`; there's no separate battery-creation step for firmware to worry about. Voltage/percentage logging (`battery_logs`) is firmware-integrated for both batteries, submitted as part of `sync`'s `batteryLogs` array — `esp32` since firmware 0.3.0, `pump` since 0.4.0. `checkEveryMs`/`checkAvgCount` **are** read by firmware — every `sync` response includes a `batteries[]` array with each battery's current values, applied fresh each cycle (no revision/confirm sync like `Device`/`SoilSettings` — the device just reads the latest value every time, there's nothing to "confirm applying"). `batteryAlertPercent` and `battery_normalization` (calibration pairs) remain purely manual — nothing reads either of them yet; they're just stored via this section's full CRUD routes for when alerting/calibration logic is built.
>
> **Hardware note:** the ESP32's own battery voltage is read on **GPIO34** (ADC1_CH6) through a 1 MΩ + 1 MΩ divider with 10 nF from the midpoint to GND. Since firmware 0.3.0 the divider's low side goes to **GPIO26** (`PIN_BAT_EN`) instead of GND: firmware drives it LOW while awake to enable the divider and leaves it a floating input during deep sleep, so the divider draws no current while the board sleeps. A reading below 2.5 V (jumper J2 removed) or above 4.5 V (divider not enabled) is treated as invalid and not sent.
>
> The pump's cell (`type: "pump"`, firmware 0.4.0+) has a copy of that divider (R3/R4 1 MΩ, 10 nF, jumper J5): midpoint on **GPIO35** (ADC1_CH7, `PIN_PUMP_BAT`), low side on **GPIO33** (`PIN_PUMP_BAT_EN`). Unlike the main divider it is switched on only for the measurement window itself (`checkAvgCount` × `checkEveryMs` of the `pump` row) and off right after, so the divider takes nothing from the pump cell the rest of the time.

#### `GET /battery`
List all batteries, newest first.

**Response 200** — `Battery[]`

#### `GET /battery/:id`
Fetch a single battery by its numeric `id`.

**Path params:** `id` (integer)

**Response 200** — `Battery`
**Response 404** — if no battery has that `id`

#### `POST /battery`
Create a battery record by hand — normally unnecessary, since a device's two `Battery` rows are provisioned automatically (see above). Useful mainly for adjusting a battery's config before the device it belongs to has ever synced, or for re-creating a row that was deleted.

**Body** (`Partial<Battery>`):
```json
{
  "type": "esp32",
  "deviceChipId": "8C94DF8B7EF8"
}
```
| Field | Type | Required | Notes |
|---|---|---|---|
| `type` | `"esp32"` \| `"pump"` | yes | `"esp32"` = the cell powering the board, `"pump"` = the pump's separate cell |
| `deviceChipId` | string | yes | FK to `devices.chipId`; the device this battery powers |
| `checkEveryMs` | int | no | defaults to `400` |
| `checkAvgCount` | int | no | defaults to `10` |
| `batteryAlertPercent` | int | no | defaults to `30` |

**Response 201** — created `Battery` (with generated `id` and defaults applied for any omitted field)

#### `PUT /battery/:id`
Update a battery. Plain field merge — no revision bump, since there's no device to sync this to yet.

**Path params:** `id` (integer)

**Body** — any subset of `Battery` fields.

**Response 200** — updated `Battery`
**Response 404** — if `id` not found

#### `DELETE /battery/:id`
Delete a battery. Cascades to its `battery_logs` and `battery_normalization` rows.

**Path params:** `id` (integer)

**Response 200**
**Response 404** — if `id` not found

---

#### `GET /battery-logs`
List battery voltage/percentage log entries, newest first.

**Query params:** `batteryId` (optional, integer) — when provided, scopes the list to that battery only; omit to list across all batteries.

**Response 200** — `BatteryLog[]`

#### `POST /battery-logs`
Submit a battery log entry directly by `batteryId`. This exists for manual/admin use (e.g. backfilling a reading taken by hand) — the ESP32 itself submits its `esp32`-battery readings through [`sync`](#post-devicesby-chipchipidsync)'s `batteryLogs` array instead, addressed by `(chipId, type)` rather than a raw `batteryId`.

**Body:**
```json
{
  "batteryId": 1,
  "volt": 3.7,
  "percentage": 82
}
```
| Field | Type | Required | Notes |
|---|---|---|---|
| `batteryId` | integer | yes | FK to `battery.id` |
| `volt` | number | yes | measured voltage |
| `percentage` | integer | yes | battery percentage (computed however the caller sees fit — the server does no computation) |

**Response 201** — created `BatteryLog`

No `PUT`/`PATCH`/`DELETE` — this is an append-only log, matching the `soil-readings`/`device-logs` pattern.

---

#### `GET /battery-normalization`
List multimeter-vs-ADC calibration pairs.

**Query params:** `batteryId` (optional, integer) — same filtering behavior as `GET /battery-logs`.

**Response 200** — `BatteryNormalization[]`

#### `GET /battery-normalization/:id`
Fetch a single calibration pair by its numeric `id`.

**Response 200** — `BatteryNormalization`
**Response 404** — if no row has that `id`

#### `POST /battery-normalization`
Record a calibration pair: a voltage measured by hand with a multimeter, paired with the raw ADC value the ESP32 read for the same battery at the same moment. This is used to build a more accurate voltage/percentage curve than trusting the ESP32's raw ADC reading alone (ADC readings on the ESP32 are known to be somewhat noisy/non-linear without calibration).

**Body:**
```json
{
  "batteryId": 1,
  "multimeterValue": 3.71,
  "esp32Value": 2650
}
```
| Field | Type | Required | Notes |
|---|---|---|---|
| `batteryId` | integer | yes | FK to `battery.id` |
| `multimeterValue` | number | yes | ground-truth voltage, measured by hand |
| `esp32Value` | integer | yes | raw ADC value the ESP32 reported at the same time |

**Response 201** — created `BatteryNormalization`

#### `PUT /battery-normalization/:id`
Update a calibration pair.

**Body** — any subset of `BatteryNormalization` fields.

**Response 200** — updated `BatteryNormalization`
**Response 404** — if `id` not found

#### `DELETE /battery-normalization/:id`
Delete a calibration pair.

**Response 200**
**Response 404** — if `id` not found

---

### 3.6 Auto-watering — pump settings, waterings

Firmware 0.4.0+. The pump runs from its **own** cell (TP4056 No. 2 → MT3608 boost at 5 V → pump), switched on the low side by an IRLZ44N whose gate is driven from **GPIO27** (`PIN_PUMP`, 100 Ω series, 10 kΩ gate pull-down keeps it off while the pin floats). With the key open the boost module is unpowered, so the pump cell gives nothing outside of watering.

**The cycle:** soil measurement ≤ `soil_settings.autoWaterPercent` → board-side checks (pump cell reading valid and ≥ `minBatteryVolt`, fewer than `maxWateringsPerDay` waterings in its own 24 h memory) → [`POST .../watering`](#post-devicesby-chipchipidwatering) → pump for `ml / flowMlPerSec` s (≤ `maxPumpSeconds`) → `report.waterings[]` → deep sleep `postWaterSleepSeconds` → the next wake measures again and sends `moistureAfter` (and never waters itself) → back to `devices.sleepSeconds`. A skipped watering is logged as `watering_skipped` (`reason` = one of the server reasons, or `pump_battery_invalid`).

**Offline:** with no Wi-Fi or no server the board waters on its cached (NVS) pump settings, queues the record in RTC memory (`offline: true`) and reports it later with `agoSec`; skips made offline arrive as one `watering_skipped` log with `offline: true, count, lastReason`. Holding BOOT for ~50 ms while the pump runs stops it early (`stoppedByButton` in the `watering` log).

#### `GET /devices/:deviceId/pump-settings`
**Response 200** — `PumpSettings` (see [4.8](#48-pump_settings)). A device that predates pump settings gets its row created with the defaults on this first read. **404** for an unknown or malformed `deviceId`.

#### `PUT /devices/:deviceId/pump-settings`
Dashboard-facing. Bumps `revision`; the board confirms via `sync`'s `pump.appliedRevision`. Only these fields are accepted (anything else in the body is ignored), and out-of-range values are rejected with **400**:

| Field | Range |
|---|---|
| `autoWaterEnabled` | boolean |
| `waterMl` | 10–2000 |
| `flowMlPerSec` | 1–200 |
| `postWaterSleepSeconds` | 60–86400 |
| `maxWateringsPerDay` | 1–8 |
| `minBatteryVolt` | 3.0–4.2 |
| `maxPumpSeconds` | 1–300 |

#### `GET /devices/:deviceId/waterings`
Watering history, newest first (by `wateredAt`). Query: `limit` (default 50, max 1000), `offset`. **Response 200:** `{ items: WateringLog[], total, limit, offset, hasMore }`.

---

## 4. Database schema

Postgres, via TypeORM. Schema is auto-synced from entities outside of production (no migrations exist yet), so this reflects the live schema.

### 4.1 `devices`

| Column | Type | Nullable | Default | Notes |
|---|---|---|---|---|
| `id` | uuid (PK) | no | generated | |
| `chipId` | varchar | no | — | **unique**; MAC-derived identifier reported by firmware (12 hex chars, no separators) |
| `name` | varchar | yes | null | human-friendly label |
| `lastSeenAt` | timestamptz | yes | null | updated on every `sync` and `report` (never by dashboard `PUT`) |
| `lastIp` | inet | yes | null | updated on `sync` when `device.lastIp` is provided |
| `firmwareVersion` | varchar | yes | null | reported by device; no OTA mechanism exists |
| `sleepSeconds` | int | no | `50` | deep-sleep interval; changing bumps `revision` |
| `isActive` | boolean | no | `false` | see [isActive — ownership and behavior](#isactive--ownership-and-behavior); changing bumps `revision` |
| `deviceLogsSeconds` | int | no | `300` | heartbeat interval for the `report`-submitted heartbeat log, used **only** while `isActive` is not `true`; editable from the dashboard via `PUT /devices/:id` |
| `revision` | int | no | `1` | server-owned config version counter |
| `appliedRevision` | int | yes | null | last revision the device confirmed applying |
| `createdAt` | timestamptz | no | now() | |
| `updatedAt` | timestamptz | no | now() | auto-updated on save |

### 4.2 `soil_settings`

| Column | Type | Nullable | Default | Notes |
|---|---|---|---|---|
| `id` | uuid (PK) | no | generated | |
| `deviceId` | uuid (FK → `devices.id`, `ON DELETE CASCADE`, **UNIQUE**) | no | — | exactly one settings row per device, DB-enforced |
| `soilDry` | int | no | `3050` | raw ADC value representing fully dry soil |
| `soilWet` | int | no | `900` | raw ADC value representing fully wet soil |
| `soilEveryMs` | int | no | `400` | sampling interval in ms |
| `soilAvgCount` | int | no | `10` | number of samples to average per reading |
| `dryAlertPercent` | int | no | `30` | threshold to trigger a "too dry" alert |
| `autoWaterPercent` | int | no | `20` | auto-watering trigger: the board runs the pump at or below this moisture (firmware 0.4.0+) |
| `revision` | int | no | `1` | bumped on every human/API update |
| `appliedRevision` | int | yes | null | last revision the device confirmed applying |
| `updatedAt` | timestamptz | no | now() | |

### 4.3 `soil_readings`

| Column | Type | Nullable | Default | Notes |
|---|---|---|---|---|
| `id` | bigint (PK) | no | auto-increment | |
| `deviceId` | uuid (FK → `devices.id`, `ON DELETE CASCADE`) | no | — | indexed together with `recordedAt` (see below) |
| `raw` | int | no | — | raw ADC reading, **averaged on-device over `soilAvgCount` samples** (from that device's `SoilSettings`) before submission |
| `percent` | smallint | **yes** | null | moisture percentage computed from the averaged `raw` value; nullable so a failed/skipped computation doesn't block submitting the reading |
| `voltage` | real | no | — | sensor voltage, also from the averaged sample |
| `minRaw` | int | no | — | calibration min at time of reading |
| `maxRaw` | int | no | — | calibration max at time of reading |
| `rssi` | smallint | **yes** | null | WiFi signal strength (dBm); nullable so a missing/failed RSSI read doesn't block submitting the reading |
| `uptimeSec` | int | no | — | device uptime in seconds at time of reading |
| `bootCount` | int | **yes** | null | device's RTC wake counter — see [`bootCount` — wake correlation](#bootcount--wake-correlation); nullable for rows predating this column |
| `recordedAt` | timestamptz | no | now() | server-set on insert |

**Indexes:** composite `(deviceId, recordedAt)` — matches the access pattern of `GET /devices/:deviceId/soil-readings` (filter by device, sort by time descending). Also `(deviceId, bootCount)` — `bootCount` is only unique per-device, not globally, so it's always queried/indexed scoped to a `deviceId`.

### 4.4 `device_logs`

| Column | Type | Nullable | Default | Notes |
|---|---|---|---|---|
| `id` | int (PK) | no | auto-increment | |
| `deviceId` | uuid (FK → `devices.id`, `ON DELETE CASCADE`, indexed) | no | — | |
| `bootCount` | int | **yes** | null | device's RTC wake counter — see [`bootCount` — wake correlation](#bootcount--wake-correlation); nullable since a log may not originate from a device wake cycle, and rows predating this column have none |
| `createdAt` | timestamptz | no | now() | |
| `info` | jsonb | no | — | freeform payload — see [event catalog](#format-conventions) in 3.4 for recommended shape |

**Index:** composite `(deviceId, bootCount)`, same rationale as `soil_readings` above.

### 4.5 `battery`

| Column | Type | Nullable | Default | Notes |
|---|---|---|---|---|
| `id` | int (PK) | no | auto-increment | |
| `type` | enum (`esp32`, `pump`) | no | — | `esp32` = the cell powering the board, `pump` = the pump's separate cell (measured since firmware 0.4.0) |
| `deviceChipId` | varchar (FK → `devices.chipId`, `ON DELETE CASCADE`) | no | — | the device this battery powers |
| `checkEveryMs` | int | no | `400` | interval between battery checks; read fresh by firmware every `sync` call |
| `checkAvgCount` | int | no | `10` | number of samples to average per check; read fresh by firmware every `sync` call |
| `batteryAlertPercent` | int | no | `30` | low-battery alert threshold; not yet read by anything |
| `createdAt` | timestamptz | no | now() | |
| `updatedAt` | timestamptz | no | now() | auto-updated on save |

### 4.6 `battery_logs`

| Column | Type | Nullable | Default | Notes |
|---|---|---|---|---|
| `id` | int (PK) | no | auto-increment | |
| `batteryId` | int (FK → `battery.id`, `ON DELETE CASCADE`, indexed) | no | — | |
| `volt` | real | no | — | measured voltage |
| `percentage` | smallint | no | — | battery percentage |
| `createdAt` | timestamptz | no | now() | server-set on insert |

### 4.7 `battery_normalization`

| Column | Type | Nullable | Default | Notes |
|---|---|---|---|---|
| `id` | int (PK) | no | auto-increment | |
| `batteryId` | int (FK → `battery.id`, `ON DELETE CASCADE`, indexed) | no | — | |
| `multimeterValue` | real | no | — | ground-truth voltage measured by hand with a multimeter |
| `esp32Value` | int | no | — | raw ADC value the ESP32 read at the same moment |

Used to build a more accurate voltage/percentage curve for a battery than trusting its raw ESP32 ADC readings alone — pair up a hand-measured multimeter reading with the corresponding raw ADC value at the same point in time, and accumulate enough pairs to fit a proper calibration curve.

### 4.8 `pump_settings`

| Column | Type | Nullable | Default | Notes |
|---|---|---|---|---|
| `id` | uuid (PK) | no | generated | |
| `deviceId` | uuid (FK → `devices.id`, `ON DELETE CASCADE`, **UNIQUE**) | no | — | one row per device |
| `autoWaterEnabled` | boolean | no | `true` | master switch |
| `waterMl` | int | no | `350` | dose per watering |
| `flowMlPerSec` | real | no | `26.67` | pump output at 5 V; measured 1000 ml in 37.5 s |
| `postWaterSleepSeconds` | int | no | `600` | deep sleep after a watering, before the control measurement |
| `maxWateringsPerDay` | int | no | `3` | rolling 24 h limit |
| `minBatteryVolt` | real | no | `3.3` | no watering below this pump cell voltage |
| `maxPumpSeconds` | int | no | `60` | cap on one pump run |
| `revision` | int | no | `1` | bumped on every dashboard update |
| `appliedRevision` | int | yes | null | last revision the board confirmed applying |
| `updatedAt` | timestamptz | no | now() | |

### 4.9 `watering_logs`

| Column | Type | Nullable | Default | Notes |
|---|---|---|---|---|
| `id` | bigint (PK) | no | auto-increment | |
| `deviceId` | uuid (FK → `devices.id`, `ON DELETE CASCADE`) | no | — | |
| `nonce` | bigint | no | — | the board's random id for the watering; dedup key for resends, match key for `wateringsAfter` |
| `bootCount` | int | yes | null | wake that ran the pump |
| `ml` | int | no | — | dose asked for |
| `seconds` | real | no | — | how long the pump actually ran |
| `flowMlPerSec` | real | yes | null | flow used for the calculation |
| `moistureBefore` / `moistureAfter` | smallint | yes | null | trigger moisture / moisture after the post-watering sleep |
| `pumpBatteryVolt` | real | yes | null | pump cell before the run |
| `offline` | boolean | no | `false` | watered on cached settings |
| `wateredAt` | timestamptz | no | — | when the pump ran (`now − agoSec`) |
| `createdAt` | timestamptz | no | now() | when the server received it |

**Indexes:** `(deviceId, wateredAt)` for the history and the 24 h count, `(deviceId, nonce)` for dedup/matching.

---

## 5. Typical ESP32 firmware flow

1. **Start of every wake cycle:** call `POST /devices/by-chip/:chipId/sync` with `{ device: { lastIp, appliedRevision, isActive }, soil: { appliedRevision }, batteryLogs? }` — see [`sync`](#post-devicesby-chipchipidsync). This single call handles first-contact registration (unknown `chipId`), the config heartbeat, the soil-settings fetch, both revision confirms, battery-log submission, and battery-config fetch all at once — there's no separate register/ping/soil-settings-GET/confirm/battery-config sequence anymore.
   - The response always contains the full `device` and `soil` objects, plus `batteries[]`. Diff `device.revision`/`soil.revision` against what was last applied; if either changed, apply the new values (`sleepSeconds`/`isActive`/`deviceLogsSeconds` from `device`, `soilDry`/`soilWet`/etc. from `soil`) and store the new revision numbers. The corresponding `appliedRevision` is sent back on a *later* `sync` call, once applied — there's no separate confirm request. `batteries[]` (each battery's `checkEveryMs`/`checkAvgCount`) has no revision to track — just apply the latest values every cycle.
   - **Check `isActive` before doing anything else this cycle** (see [isActive — ownership and behavior](#isactive--ownership-and-behavior)). Treat anything other than `isActive === true` as inactive:
     - **Inactive:** skip sleep/measurement entirely; submit a heartbeat log (no `readings`) via `report` every `deviceLogsSeconds` seconds and loop.
     - **Active:** proceed with the normal flow below.
2. **After each sensor sample, and/or whenever there's a log to send:** call `POST /devices/by-chip/:chipId/report` with `{ readings?: [...], logs?: [...] }` — see [`report`](#post-devicesby-chipchipidreport). Both arrays are optional; send whichever ones have data this cycle (a sensor-disconnected cycle sends only `logs`, a normal cycle sends one reading in `readings` plus a `cycle_done` log, a retry-recovery cycle can resend a buffered reading alongside a fresh one). Include `bootCount` on each entry — see [`bootCount` — wake correlation](#bootcount--wake-correlation).
   - Check the response's `readings.rejected`/`logs.rejected` counts before clearing any RTC-buffered retry data — only drop the buffer once the corresponding count is `0`.
   - Check the response's `device.isActive`/`device.revision` — if `isActive` has flipped to `false` or `revision` has moved since the last `sync`, treat it as a mid-cycle deactivation/config change rather than waiting for the next wake to notice.
3. **On boot, errors, or while inactive:** include a log entry in `report`'s `logs` array — `{ info: { chipId, level, event, ... }, bootCount? }`. See the [event catalog](#format-conventions) in 3.4 for what's worth logging (boot/reset reason, Wi-Fi/ping failures, sensor disconnects, power/memory stats, config-applied confirmations).
4. **Auto-watering (firmware 0.4.0+):** after the soil measurement, if moisture ≤ `soil.autoWaterPercent` — see [3.6](#36-auto-watering--pump-settings-waterings) for the checks, [`watering`](#post-devicesby-chipchipidwatering) for the request, and `report.waterings[]` for the record.
5. **Before deep sleep (active mode only):** use `sleepSeconds` from the device config (received via `sync`) to set the sleep timer — or `pump.postWaterSleepSeconds` right after a watering.

---

## 6. Known gaps (worth knowing before relying on this API)

- No authentication — anyone who can reach the server can read/write any device's data. Do not expose this server directly to the public internet without adding auth.
- No request validation — malformed field names/types are silently accepted by the DB layer (or fail at the DB level with a generic error), aside from the minimal per-entry field checks `report`/`sync`'s `batteryLogs` do (missing required fields, unknown battery `type`).
- No OTA/firmware-update endpoint — `firmwareVersion` is just a reported string.
- `isActive`'s ownership rule (device-only for `false→true` at first contact, either side for `true→false` at any time) is a firmware/operator convention, not something the server enforces beyond the one hard rule it does apply (an existing device's `isActive` can never be flipped to `true` via `sync`'s body — see [isActive — ownership and behavior](#isactive--ownership-and-behavior)). `PUT /devices/:id` will still accept `{ "isActive": true }` from any caller; nothing stops a non-firmware caller from activating a device that way.
- `device_logs.info` has no fixed schema at the database level — the [format conventions](#format-conventions) (common envelope, `event` as machine identifier) are a convention firmware/backend must both follow; nothing enforces it server-side.
- **No "collect mode" feature.** `SoilSettings` previously had an inert `collectMode` boolean with no defined behavior on either side — it has been removed from the entity, DB schema, and dashboard UI. This functionality should **not** exist on the backend or on the ESP32 firmware; do not reintroduce it without first defining what it's actually supposed to do.
- **Battery `checkEveryMs`/`checkAvgCount` are read by firmware; `batteryAlertPercent` and calibration are not.** `battery`'s config fields are read fresh by the device on every `sync` (via the response's `batteries[]` array) and voltage logging (`battery_logs`) is written by firmware — for both `type: "esp32"` (GPIO34) and, since firmware 0.4.0, `type: "pump"` (GPIO35). `batteryAlertPercent` and `battery_normalization` (calibration pairs) are stored but read by nothing yet — pure manual bookkeeping via their CRUD routes, same as `battery` itself has no revision/confirm sync (config changes just take effect on the device's next `sync` call, no "applied" confirmation exists for this resource). See [3.5](#35-battery-tracking--battery-battery-logs-battery-normalization).
- **Breaking change, not backward-compatible.** `sync`/`report` replaced `register`/`ping`/`confirm`/`soil-settings confirm`/`soil-readings POST`/`device-logs POST` outright — those routes no longer exist. Firmware and backend must be updated together; there is no dual-running period where both old and new routes are live.
