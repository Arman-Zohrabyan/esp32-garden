# Connecting the ESP32 to the backend + splitting the firmware into modules

## Implementation status (August 15, 2026)

The project builds: **Flash 52.8%** (1038013 of 1966080), RAM 15.2%.

### Switch to sync/report (August 15, 2026)

The backend collapsed six endpoints into two. The change is incompatible: the old
`register`/`ping`/`confirm`/`soil-settings`/`soil-readings`/`device-logs`
no longer exist on the server, so the firmware cannot be rolled back.

| Before | After |
|---|---|
| `POST /devices/register` | — (created inside `sync`) |
| `POST /devices/:id/ping` | `POST /devices/by-chip/:chipId/sync` |
| `GET /devices/:id/soil-settings` | ↑ same, `soil` block in the response |
| `PATCH .../confirm` ×2 | ↑ same, `appliedRevision` in the request body |
| `POST /devices/:id/soil-readings` | `POST /devices/by-chip/:chipId/report` |
| `POST /device-logs` | ↑ same, `logs` array |

Requests per active cycle: **was 4–6, now 2**. In the worst case (database
wipe + brownout + resend) it was 9–10, now still 2.

What changed in the firmware besides the addresses:

- **Addressing by `chipId`, not by UUID.** The board always has a `chipId`, even before
  it first goes online. The server registers an unknown `chipId` by itself,
  so the re-registration branch and 404 handling are gone — a database wipe
  is now indistinguishable from the first power-on. The UUID is only needed for
  `PUT /devices/:id` (activation from the page).
- **`ReportBatch`** accumulates logs and readings over a cycle and sends them in one request.
  Previously `boot` + `wifi_failed` + `cycle_done` were three requests.
- **`bootCount` in every record.** A deferred reading carries the wake-up on
  which it was *taken*, not the one on which it was sent (`rtcPendBoot`).
- **The battery goes inside `sync`** as the `batteryLogs` array.
- **Checking the `accepted`/`rejected` counters.** The RTC buffer is cleared only
  when `rejected == 0`; HTTP 200 by itself guarantees nothing.
- **Catching deactivation in the `report` response.** The server might have cleared activity
  while the readings were running (~20 s) — the board learns about it before sleeping, not a cycle later.
- **`rtcFailStreak` also grows when the server is unreachable,** not only when
  Wi-Fi fails to come up. Otherwise, with the server down, the board would sleep forever.

**NOT TESTED ON A LIVE BOARD** — the ESP32 was disconnected from USB during implementation.
None of the steps in the "Verification" section below has been checked. Before
relying on the firmware, the whole scenario must be run: from `chipId` in the serial port to
a full cycle with sleep.

Structure after the split (before: 609 lines in one file):

| File | Lines | Contents |
|---|---|---|
| `src/main.cpp` | 590 | setup/loop, active cycle, synchronization |
| `src/api.cpp` | 384 | sync/report, batches, transport |
| `src/web.cpp` | 290 | portal, dashboard, /activate |
| `src/soil.cpp` | 197 | sensor, collectWindow, open-circuit detection |
| `src/net.cpp` | 153 | Wi-Fi, AP, scan, mDNS garden.local |
| `src/state.cpp` | 136 | g_dev/g_soil, NVS, RTC |
| `src/battery.cpp` | 103 | charge reading on GPIO34, Li-ion curve |
| `src/sleepmgr.cpp` | 64 | chipId, resetReason, deep sleep |

## Context

Right now the board sends a GET ping to the hardcoded `192.168.11.84:3000` ([main.cpp:30](src/main.cpp#L30)) — the address doesn't work. A real backend has appeared, `esp32.novacoteam-server.com` (accessed over `http://`, see decisions below), with revision-based config synchronization, device registration, telemetry and logs. The task is to teach the firmware to work with it.

Two problems are solved along the way:

1. **All code in one file** — 609 lines in [main.cpp](src/main.cpp). With the new logic it will become ~1000+. We split it into modules.
2. **Two mechanisms for one thing** — the local `collect/on` in flash ([main.cpp:391](src/main.cpp#L391)) and the server-side `isActive` do the same thing. We keep one.

Result: the board registers itself, gets calibration and schedule from the server, sends averaged readings, logs errors, and is controlled remotely — while remaining accessible locally.

## Verified facts (not assumptions)

- The server is alive: `GET /devices` → `200 []`, `/device-logs` → `200`, `/devices/8C94DF8B7EF8` → `404` (not registered yet), `POST /devices/register` with `{}` → `500`.
- **HTTP without TLS answers 200 with no redirect** — we use it (user's decision).
- Board MAC: `8C:94:DF:8B:7E:F8` → `chipId = "8C94DF8B7EF8"` (12 hex, not 6).
- Flash is **76.2%** used (999257 of 1310720). `min_spiffs.csv` gives app0 = 0x1E0000 = 1920 KB → usage will drop to ~51%.
- NVS in both layouts is at `0x9000` with size `0x5000` → **the saved Wi-Fi will survive the partition change**.
- mbedtls (81.4 KB) and `WiFiClientSecure` are already linked into the current binary — dropping TLS doesn't save flash, but it saves handshake time and RAM.

## User's decisions

| Question | Decision |
|---|---|
| Transport | Plain HTTP (no TLS) — rationale below |
| `pingTimeoutSeconds` | Server policy for "when to consider the board offline", not a client socket timeout |
| Structure | Split into modules right away |
| `collectMode` | Replace with `isActive` |
| Logs | Everything from the catalog |

**Why HTTP, not HTTPS.** Flash has nothing to do with it — contrary to the initial assumption, mbedtls (81 KB) and `WiFiClientSecure` are **already linked** into the current binary, because `HTTPClient` in arduino-esp32 pulls them in unconditionally. The space for TLS is already paid for regardless of use.

The real difference is time and RAM: the TLS handshake adds 1-3 s and ~40-50 KB of peak heap to every request. In a cycle where the radio already runs ~4 s, that is a noticeable addition to battery drain.

The cost of this decision: `isActive`, `sleepSeconds` and calibration travel over the network **in plain text**. Someone on the same Wi-Fi can read or tamper with them — for example, put the board to sleep for a long time. For a home network this is accepted as a tolerable risk. If the board moves to someone else's/a public network, the decision must be revisited; `setInsecure()` would then protect only against passive eavesdropping, not tampering — root CA verification will be needed.

## File structure

```
include/  config.h  state.h  soil.h  net.h  api.h  web.h  sleepmgr.h
src/      main.cpp  state.cpp soil.cpp net.cpp api.cpp web.cpp sleepmgr.cpp
```

`sleepmgr`, not `sleep` — xtensa newlib has a system `<sleep.h>`, so the name conflicts.

PlatformIO compiles everything in `src/` automatically; `include/` is already in the include path.

**What moves where:**

| Module | From main.cpp | Functions |
|---|---|---|
| `soil` | 134-218 | `readSoil`, `soilWord`, `pollSoil` (inactive mode) + new `collectWindow` (active), `sensorOk` |
| `net` | 51-128, 547-571 | `htmlEscape`, `startScan`, `pollScan`, `waitConnect`, `connectTo`, `manageAP` |
| `web` | 222-382, 500-542 | `handleRoot`, `handleSave`, all routes + new `/activate` |
| `sleepmgr` | 389-462 | `collectMode` → rewritten as `activeCycle`, `chipId`, `bootCount`, `resetReason` |
| `api` | new | all communication with the backend |
| `state` | 11-46 | global variables → structs `g_dev`, `g_soil` + RTC |

`main.cpp` stays at ~150 lines: only `setup()`/`loop()`.

**`pollSoil` and `collectWindow` are different functions; don't reuse one for both.** `pollSoil` is non-blocking, called from `loop()` in inactive mode, and accumulates the window across calls. `collectWindow(count, gapMs)` is synchronous: it collects the whole window in one call via `delay()` and returns the result. Merging them would mean either blocking `loop()` in inactive mode or needlessly complicating the active cycle with a state machine.

**`dryAlertPercent`** comes from the server and is cached in NVS, but the firmware **does not act on it** — the threshold exists for server analytics and the dashboard. The board only reports `percent`; the watering decision is made on the server. Recorded deliberately so that nobody goes looking for "lost" logic during implementation.

## Key technical decisions

### Config sync first, everything else after

**This is the main invariant of the cycle.** `revision` can change at any moment, so right after waking the board must find out the current settings — and only then act on them. Readings with stale calibration give wrong percentages, and sleeping by a stale `sleepSeconds` breaks the schedule.

Order in active mode:

```
wake → Wi-Fi
     → POST /devices/:id/ping        → Device: isActive, sleepSeconds, deviceLogsSeconds
     → GET  /soil-settings           → SoilSettings: soilEveryMs, soilAvgCount, soilDry, soilWet
     → PATCH both /confirm (if revision changed)
     → check isActive (fail-safe: anything but true → go to web mode, don't sleep)
     → ONLY NOW: reading, then [pause soilEveryMs → reading] ×(soilAvgCount-1)
     → POST /soil-readings
     → deep sleep by the current sleepSeconds
```

Exactly `soilAvgCount-1` pauses, not `soilAvgCount`: a pause after the last reading would delay the send for nothing. With defaults that's 9 pauses × 2 s = 18 s, not 20.

`soilEveryMs` and `soilAvgCount` mean literally: the interval between readings and their count. No local pause constants — the server is the single source of truth.

**Until all `soilAvgCount` readings are done, the board doesn't send the request and doesn't go to sleep.**

**Wi-Fi comes up BEFORE the readings** — otherwise it's unknown how many readings to take and with what pause. This changes the earlier decision (readings first) and creates a problem: the running transmitter induces interference on the high-impedance ADC input. Mitigation — after the config sync, for the duration of the reading window, switch the radio to `WIFI_PS_MAX_MODEM` or `WiFi.disconnect()` and reconnect before sending. **Needs checking on a live board:** compare the `maxRaw-minRaw` spread with and without the radio (reference with Wi-Fi off — under 20 units).

If the config didn't arrive (ping/settings failed), there's nothing to base readings on, so fall back: the NVS cache, and if the cache is empty, `DEF_*` from config.h. The board is never left without settings.

Consequence with defaults (2000 ms × 10): awake ~18 s for readings + ~2-4 s for networking, i.e. ~22 s out of every 72 with `sleepSeconds=50`. Adjustable from the server: raise `sleepSeconds` or lower `soilEveryMs` (averaging suppresses ADC noise, not moisture fluctuations — a spread of 3233..3251 is digitization noise).

Watchdog: the reading loop with 2 s pauses must run through `delay()` — it feeds the WDT in the Arduino framework. No blocking busy-wait `while` loops.

### RTC memory survives deep sleep

`RTC_DATA_ATTR`: `rtcBootCount`, `rtcFailStreak`, `rtcMagic` (block validity) + `rtcPendRaw`/`rtcPendPct`/`rtcPendMin`/`rtcPendMax` — an **already complete** averaged window that failed to send (not an accumulation; the window is always collected in full within one wake period).

**Gotcha:** the `RTC_DATA_ATTR` attribute must be on both the `extern` declaration in `state.h` and the definition in `state.cpp` — otherwise the variable ends up in `.bss` and is zeroed during sleep. Verified by `rtcBootCount` growing between wake-ups.

### NVS

The `collect` namespace is removed with a migration: if `collect/on == true`, treat it as `isActive = false` (fail-safe) and erase it. Otherwise after the update the board would fall asleep in a state unknown to the server.

| Namespace | Keys |
|---|---|
| `wifi` | `ssid`, `pass` (untouched) |
| `dev` | `id`, `active`, `rev`, `sleepSec`, `logSec` |
| `soil` | `dry`, `wet`, `everyMs`, `avgCnt`, `dryPct`, `rev` |

NVS keys are ≤ 15 characters — hence `sleepSec`, not `sleepSeconds`. Write only on an actual change (flash wear: 100k cycles, and with `sleepSeconds=50` that's ~1700 wake-ups per day).

### isActive — the order of operations is critical

**Activation (false→true), only from web.cpp:**
```
click → PUT /devices/:id {"isActive":true}
      → if !ok: show an error, save NOTHING, stay in web mode
      → if ok:  NVS active=true → serve HTML → delay(300) → deep sleep
```
Server first, then NVS. Otherwise the board falls asleep in a state the server doesn't know about, and it will never send `isActive=false` — the only way to wake it will be the button.

**Deactivation (true→false):**
- from the server: ping returned `isActive:false` → NVS → **don't sleep**, bring up AP+web → confirm;
- with the BOOT button: save `active=false` to NVS **immediately** (the emergency stop must work without a network), then a best-effort `PUT`. The order is the reverse of activation — on purpose.

**Fail-safe parsing:** `dev.isActive = doc["isActive"].is<bool>() && doc["isActive"].as<bool>();` — exactly like this, not `doc["isActive"] | false` (the latter gives `true` on the string `"false"`).

### Inactive mode doesn't sleep

`isActive == false` → Wi-Fi + AP + web page stay live (needed for local activation — the only path false→true), heartbeat to `/device-logs` every `deviceLogsSeconds`.

Same invariant as in active mode: `ping` goes along with the heartbeat, and if `revision` changed, the config is applied immediately. That's how the board learns about `isActive=true` set from the server and about new `deviceLogsSeconds` without waiting for a reboot.

This costs more power than active mode. Unavoidable: a sleeping board can't serve the activation page.

### ArduinoJson 7 with a filter

`bblanchon/ArduinoJson@7.0.4` (exact version, not `^`). In v7 `JsonDocument` manages its own size — the whole class of "underestimated `StaticJsonDocument<N>`" bugs goes away.

Parse **from the stream** `http.getStream()`, not from `http.getString()` — otherwise the body is copied into a `String` and peak RAM doubles.

A filter on the `/ping` response drops `createdAt`/`updatedAt`/`name`/`lastSeenAt` — the firmware doesn't need them.

## Files

**[platformio.ini](platformio.ini)**
```ini
board_build.partitions = min_spiffs.csv
lib_deps = bblanchon/ArduinoJson@7.0.4
build_flags = -DFW_VERSION=\"0.2.0\"
monitor_filters = esp32_exception_decoder, time
```

**[include/config.h](include/config.h)** — gets extended. `DRY`/`WET` → `DEF_SOIL_DRY`/`DEF_SOIL_WET` (now these are defaults the server can override). Added: `API_BASE_URL` (http, not https), timeouts, Device/SoilSettings defaults matching the server's, sensor detection thresholds, pins.

**Thresholds for detecting a disconnected sensor** (measured: when connected, spread <20, mean ~3240-3289 in air; open circuit **was not measured**):
```c
#define SOIL_DISCONNECT_SPREAD  300   // larger spread -> floating input
#define SOIL_RAW_MIN_VALID      200   // below -> short to ground
#define SOIL_RAW_MAX_VALID      4000  // above -> open circuit
#define SOIL_DISCONNECT_STRIKES 2     // consecutive windows before triggering
```
The main criterion is the spread, not the absolute value: with an open circuit the input is high-impedance and picks up interference. The `200/4000` bounds catch the second case — the input pinned to a rail (the spread is then near zero instead). They need calibration: unplug the connector, watch the log for 30 s.

**api.h** — `ApiStatus{Ok, NoWifi, Transport, HttpError, ParseError}` + `ApiResult{status, httpCode}`. A bare `bool` isn't enough: it loses the distinction "network didn't come up" / "404" / "broken JSON", which is exactly what the handling branches on.

Functions: `apiRegister`, `apiPing(dev, bool& fullConfig)`, `apiConfirmDevice`, `apiPutIsActive`, `apiGetSoilSettings`, `apiConfirmSoil`, `apiPostReading`, `apiPostLog(level, event, extraJson)`.

A private helper `doRequest()` — the single place where timeouts are set and `http.end()` is **always** called (otherwise the heap leaks within a few cycles).

## Log catalog

The skeleton in `info` everywhere: `chipId`, `level`, `event`, `uptimeSec`.

| Event | level | Extra fields |
|---|---|---|
| `boot` | info | `bootCount`, `reason` (DEEPSLEEP/PANIC/BROWNOUT/WDT/POWERON), `fwVersion` |
| `heartbeat` | info | `freeHeap`, `rssi`, `soilPercent` |
| `wifi_failed` | warn | `ssid`, `wifiStatus`, `missedCycles` |
| `ping_failed` | warn | `httpCode`, `apiStatus` |
| `sensor_disconnected` | error | `raw`, `minRaw`, `maxRaw`, `spread` |
| `cycle_done` | info | `freeHeap`, `minFreeHeap`, `awakeMs` |
| `config_applied` | info | `revision`, `prevRevision`, `changed` |

**The `wifi_failed` paradox:** a "no Wi-Fi" event can't be sent over Wi-Fi. We don't try — we accumulate `rtcFailStreak` in RTC and on the next successful connection send one log with `missedCycles: N`.

## Error handling

Principle: **any network failure returns the board to an accessible state**, never to endless sleep.

| Failure | Response |
|---|---|
| Wi-Fi didn't come up | `rtcFailStreak++`. <5 — keep sleeping; ≥5 — reset `active=false`, wake into web mode |
| Registration failed | Work locally, retry every 60 s. **Don't sleep** — without an `id` there's nothing to send and nothing to control it with |
| Ping failed | Log deferred. Take readings using the **cache** (`soilAvgCount`/`soilEveryMs`/calibration from NVS), still try to send the data, sleep by the cached `sleepSeconds` |
| Ping returned 404 | Board deleted on the server → erase `dev.id` → re-register |
| Settings didn't arrive | NVS cache; if empty — `DEF_*` from config.h |
| POST reading failed | Completed window in RTC (`rtcPend*`), send it next cycle as a separate record; drop it after 5 failures |

## Implementation order

Infrastructure first with no behavior change, then functionality — at every step there's a working board to roll back to.

1. **platformio.ini** — partitions + ArduinoJson, code untouched. *Check:* Flash ~51%, the board starts, Wi-Fi wasn't lost.
2. **config.h + state.{h,cpp}** — constants and structs. *Check:* behavior identical.
3. **soil.{h,cpp}** — extract the sensor + `collectWindow` + disconnect detection. *Check:* averaging log as before; unplug the sensor → calibrate the thresholds.
4. **net.{h,cpp}** — Wi-Fi, scan, AP. *Check:* portal, connection, AP shutdown after 60 s.
5. **web.{h,cpp}** — page and routes. *Check:* the page renders, main.cpp ~150 lines. **Checkpoint: everything is split, functionality unchanged.**
6. **sleepmgr + RTC + chipId** — `esp_efuse_mac_get_default`, `bootCount`, `esp_reset_reason`. *Check:* `chipId=8C94DF8B7EF8` in the serial port (double-check — the MAC byte order is easy to mix up), `bootCount` grows after sleep.
7. **api.cpp: register + postLog** — these two first. *Check:* `POST /devices/register → 200`, `id=<uuid>`; `curl GET /devices/8C94DF8B7EF8` now returns 200 instead of 404; `boot` log on the server.
8. **ping + config + soil-settings + confirm**, still **without sleep**. *Check:* change `sleepSeconds` on the server → `config_applied revision=2`; `appliedRevision` catches up with `revision`. The trickiest step — debug it with the dashboard live.
9. **isActive + deep sleep + postReading** — the full cycle.

## Verification

`pio device monitor -p /dev/cu.usbserial-0001 -b 115200` (or `pio run -t upload -t monitor`). `monitor_filters = esp32_exception_decoder` decodes panics into function names.

End-to-end scenario for step 9:

1. Inactive mode: dashboard open, heartbeat every 10 s in the serial port and on the server.
2. Click «Начать сбор» ("Start collection") → in the serial port `PUT isActive=true → 200`, then `saved to NVS`, then `deep sleep`. **Exactly in this order.**
3. Every cycle, in the serial port, strictly in this order: `wake #N` → `ping → 200` → `soil-settings → 200` → `confirm` → reading window → `POST reading → 200` → `cycle_done awakeMs=…` → sleep. **The order is mandatory: config before readings.**
4. Calibration freshness check: while the board sleeps, change `soilDry` on the server → on the next wake-up the serial port shows `config_applied` and percentages are recalculated with the new value **in the same cycle**, not one later.
5. Schedule check: change `soilAvgCount` to 3 → the board takes 3 readings in the same cycle, not 10.
6. On the server `PUT {"isActive":false}` → on the next wake-up the board doesn't fall asleep, brings up the AP and web.
7. Emergency stop: hold BOOT during wake-up → `active=false` even with the router off.
8. Offline: turn off the router → `rtcFailStreak` grows → after 5 cycles the board goes into web mode on its own. Separately: make sure the readings then use the NVS cache, not zeros.

Separately — heap drift: 30+ wake-ups, compare `minFreeHeap` from `cycle_done` at the start and at the end. Growth would point to an unclosed `HTTPClient`.

## Risks

- **Watchdog.** Network calls in web route handlers block the HTTP server. For `/activate` this is acceptable — the board goes to sleep right after it.
- **RAM with AP+STA+WebServer.** In active mode (before sleep) don't bring up the AP and web at all, only STA — as already done in the current `collectMode()` ([main.cpp:421](src/main.cpp#L421)).
- **`min_spiffs` shrinks SPIFFS to 128 KB.** It's unused right now, but a local history buffer won't fit there anymore.
- **Debugging with deep sleep.** With defaults the board is awake ~22 s out of 72 — a sufficient window for reflashing. But if `soilAvgCount`/`soilEveryMs` are lowered and `sleepSeconds` raised on the server, the window shrinks to a couple of seconds. If the board goes into an unreachable cycle: the BOOT button during wake-up, and as a last resort `esptool.py erase_flash` (erases NVS along with the `active` flag and the saved Wi-Fi).
- **Order in `setup()`.** First — reading BOOT (emergency stop), second — the NVS migration, then the rest. Any logic before the BOOT check risks crashing the board before anyone can stop it.
- **`sleepSeconds=50` / `deviceLogsSeconds=10`** — debug defaults on the server. Battery use needs 600. Not blocking, but easy to forget.

## What remains open

- There are no migrations on the backend (`synchronize: true`) — the first field rename will drop the column. Outside the firmware's scope.
- The `sleepSeconds=50` / `deviceLogsSeconds=10` defaults on the server are for debugging; when moving to battery they need to be raised (for `sleepSeconds` aim for 600).
