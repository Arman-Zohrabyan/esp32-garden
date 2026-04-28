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
