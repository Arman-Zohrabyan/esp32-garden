# ESP32 Garden

Firmware for a battery-powered ESP32 that sits in a plant pot. It measures soil
moisture and battery charge, sends the readings to a backend, and waters the
plant on its own with a small pump.

Most of the time the board is in deep sleep. It wakes on a timer, gets its
settings from the server, takes readings, waters if the soil is dry, reports,
and goes back to sleep. If there is no network, it keeps measuring, stores the
readings in flash, and sends them once it can reach the server again.

Firmware version: **0.4.0** (set in [platformio.ini](platformio.ini)).

## Features

- **Soil moisture**: capacitive sensor on an ADC1 pin. The sensor gets power from
  a GPIO only while a reading is taken, so the electrodes don't corrode. Each
  reading averages a window of samples, and the firmware detects a disconnected
  or shorted sensor.
- **Battery monitoring**: separate readings for the board's Li-ion cell and the
  pump's cell. Each one goes through a switchable voltage divider that draws no
  current during sleep.
- **Auto-watering**: when moisture drops to the threshold, the board asks the
  server for permission and a dose, then runs the pump for the computed time.
  A daily limit and a hard cap on pump run time apply even without a network.
- **Server-driven config**: calibration, schedule, and pump settings come from
  the backend, are versioned by revision, and are cached in NVS.
- **Offline buffer**: up to 30 unsent readings stay in NVS through brownouts
  and power loss.
- **Low-battery protection**: below 3.45 V the board does not turn on Wi-Fi or
  the pump, and sleeps 12 hours at a time until the cell is charged above 3.6 V.
- **Local web UI**: a Wi-Fi setup portal and a status dashboard at
  `http://garden.local`.

## Hardware

Board: `esp32dev` (Espressif ESP32 Dev Module).

| GPIO | Role |
|---|---|
| 2  | Blue LED |
| 0  | BOOT button: emergency stop when held during wake-up |
| 32 | Soil sensor signal (ADC1) |
| 25 | Soil sensor power |
| 34 | Board cell voltage, divider midpoint (ADC1, input-only) |
| 26 | Board cell divider switch (LOW = on) |
| 35 | Pump cell voltage, divider midpoint (ADC1, input-only) |
| 33 | Pump cell divider switch (LOW = on) |
| 27 | Pump MOSFET gate (HIGH = pump running) |

Power and pump circuit, in short:

- **Board:** Li-ion cell → TP4056 → XC6220 (3.3 V) → 3V3 pin.
- **Pump:** its own cell and TP4056 → MT3608 (5 V) → pump. The low side is
  switched by an IRLZ44N, with a 100 Ω gate resistor and a 10 kΩ pull-down.
- **Voltage dividers:** 1 MΩ + 1 MΩ with 10 nF from the midpoint to ground. The
  bottom resistor goes to a GPIO, not to ground, so the divider draws current
  only while it is enabled.

All sensor readings use ADC1 on purpose: ADC2 can't be read while Wi-Fi is on.
The comments in [include/config.h](include/config.h) explain each choice.

## How it works

The board has two modes, chosen by the `isActive` flag on the server.

### Inactive mode (web mode)

The board stays awake with Wi-Fi, its own access point, and the web UI running.
Every `deviceLogsSeconds` it syncs with the server and sends a heartbeat. This
is the only mode where the board can be set up and activated, which is why it
doesn't sleep.

### Active mode (sleep cycle)

Every wake-up runs the same steps in this order:

1. If BOOT is held, stop: set `isActive=false` locally and stay awake.
2. Read both batteries before the radio turns on, because the transmitter's
   current draw pulls the cell voltage down.
3. Connect to Wi-Fi.
4. `sync`: get the current config. Settings must be current **before** any
   reading is taken.
5. If the server has turned the board off, switch to web mode.
6. Take the soil reading window (`soilAvgCount` readings, `soilEveryMs` apart).
7. Water if needed.
8. `report`: send readings, logs, and waterings in one request.
9. Deep sleep for `sleepSeconds`, or for `postWaterSleepSeconds` after watering.
   The next wake-up then takes the follow-up reading.

When there is no network, the board still takes the reading, stores it in the
buffer, and waters using the cached settings. It switches to web mode only when
the 30-reading buffer is full.

## Getting started

### Build and flash

You need [PlatformIO](https://platformio.org/). Connect the board over USB.

```bash
pio run -t upload        # build and flash
pio device monitor       # serial log, 115200 baud
```

In VS Code you can use the PlatformIO toolbar instead: **Upload** (→) and
**Serial Monitor** (plug icon). The monitor decodes crash addresses into
function names (`esp32_exception_decoder`) and adds timestamps. The serial log
is in Russian.

### First setup

1. On first boot the board starts the **`ESP32-Garden`** access point
   (password `12345678`). Connect to it and open `http://192.168.4.1`.
2. Pick your 2.4 GHz network, enter the password, and press «Подключить»
   ("Connect"). The board saves the network to flash and reconnects to it on
   every boot.
3. The dashboard is now at `http://garden.local`. It shows moisture, battery,
   registration status, RAM, flash, uptime, Wi-Fi signal, and chip temperature.
   The access point turns off 60 seconds after the board connects, and comes
   back if the connection drops.
4. The board registers with the server on its first `sync`. No manual step is
   needed: the server creates any `chipId` it hasn't seen before.

### Start and stop

- **Start:** press «▶ Активировать устройство» ("Activate device") on the
  dashboard. The board tells the server first, then saves the flag and goes to
  sleep. The server can also activate a board that's in web mode; the board
  picks this up on its next heartbeat.
- **Stop from the server:** set `isActive=false`. The board stays awake at its
  next wake-up, or right after the current cycle if the change arrives while
  it is running.
- **Emergency stop:** hold **BOOT** while the board wakes up. This works with
  no network.
- **Forget Wi-Fi:** «Отключиться от сети» ("Disconnect from network") clears the
  saved network and returns the board to the setup portal.

### LED

| LED | Meaning |
|---|---|
| Steady on | Web mode, connected to the router |
| Off | Not connected, or asleep |
| Blinking once a second | Awake in an active cycle |
| Two long blinks at wake-up | BOOT emergency stop accepted |

## Backend

The board talks to `http://esp32.novacoteam-server.com`, set as `API_BASE_URL` in
[include/config.h](include/config.h). All requests use the chip ID (the MAC
without separators, e.g. `8C94DF8B7EF8`):

| Request | When |
|---|---|
| `POST /devices/by-chip/:chipId/sync` | Start of every cycle: registration, config, applied revisions, battery readings |
| `POST /devices/by-chip/:chipId/report` | End of the cycle: soil readings, logs, waterings |
| `POST /devices/by-chip/:chipId/watering` | Just before the pump runs: permission and dose |
| `PUT /devices/:id` | Activation from the dashboard (`isActive: true`) |

The firmware clears buffered data only when the server's `rejected` counter is
0, not just on HTTP 200. The full API, the database schema, and the log event
catalog are in [ESP32_API.md](ESP32_API.md).

> **Plain HTTP.** TLS would add 1–3 s and about 40–50 KB of peak heap to every
> request, which matters on battery. The trade-off is that config and readings
> travel unencrypted, so anyone on the same network can read or change them.
> This is acceptable on a home network. On any other network, switch to HTTPS
> with root CA verification. The backend has no authentication either.

## Configuration

Real settings come from the server. The `DEF_*` values in
[include/config.h](include/config.h) apply only until the board's first
successful sync. That file also holds the pins, thresholds, and timeouts.

Things to tune for your own hardware:

- **Soil calibration:** `DEF_SOIL_DRY` (sensor in air) and `DEF_SOIL_WET`
  (sensor in water). The values differ from one sensor to the next. Change them
  on the server, or here as defaults.
- **Battery calibration:** `BATTERY_CALIB` and `PUMP_BAT_CALIB`, each computed
  as multimeter voltage ÷ logged voltage.
- **Disconnected-sensor thresholds:** `SOIL_DISCONNECT_*` and
  `SOIL_RAW_*_VALID`. The open-circuit case hasn't been measured yet. To
  calibrate, unplug the sensor and watch the log for 30 seconds.
- **Sleep interval:** the server's defaults (`sleepSeconds=50`,
  `deviceLogsSeconds=10`) are for debugging. Use about 600 s on battery.

## Project layout

```
include/config.h     pins, thresholds, defaults: the only file to edit for hardware changes
src/main.cpp         setup()/loop(), the active cycle, mode switches
src/api.cpp          sync / report / watering requests, ReportBatch
src/state.cpp        g_dev / g_soil / g_bat / g_pump, NVS cache, RTC variables
src/soil.cpp         moisture sensor, reading windows, disconnect detection
src/battery.cpp      cell voltage, Li-ion discharge curve, smoothing
src/watering.cpp     auto-watering decision, pump run, daily limit, RTC queue
src/readings.cpp     NVS ring buffer for offline readings
src/net.cpp          Wi-Fi, access point, scan, mDNS (garden.local)
src/web.cpp          setup portal, dashboard, /activate
src/sleepmgr.cpp     chip ID, reset reason, deep sleep, LED blink timer
PLAN.md              design notes, decisions, verification checklist
ESP32_API.md         backend API reference
```

Build settings in [platformio.ini](platformio.ini):

- `min_spiffs.csv` partitions give the app 1920 KB. NVS stays at the same
  address, so saved Wi-Fi survives the partition change.
- ArduinoJson is pinned to `7.0.4` so builds are reproducible.

## Troubleshooting

- **Board is stuck in the sleep cycle:** hold BOOT while it wakes. As a last
  resort, run `esptool.py erase_flash`. This also clears the saved Wi-Fi and
  every cached setting.
- **Flashing fails because the board is asleep:** reflash during a wake-up
  window, or hold BOOT so it stays in web mode.
- **Readings never arrive:** look in the serial log for the `sync`/`report`
  status (`no_wifi`, `transport`, `http_error`, `parse_error`) and the HTTP code.
- **`BROWNOUT` reset reason in logs:** the cell or wiring can't supply the
  current Wi-Fi needs at its peak. Check the battery and connections.

[PLAN.md](PLAN.md) lists the end-to-end checks to run on a live board.
