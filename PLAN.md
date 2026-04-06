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

