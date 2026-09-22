# How it works

METF is an HTTP-controlled test bench: the ESP board is wired to a device under test (DUT) and exposes GPIO, I2C, the DUT's serial log and an NTP server over HTTP. The main DUT is the Waterius device: several behaviours (NTP request/reply format, log buffer size) are sized and checked against its firmware.

Platform code is split with `#ifdef ESP32` / `#ifdef ESP8266`. NTP is ESP32-only.

## The classes and what each one is allowed to know

`main.cpp` only wires things together: it creates the server, the network and the bench routes, and calls `begin()` and `loop()` on them. Everything else lives in a class that knows as little as it can get away with.

| Class | File | Knows about | Deliberately does not know about |
|---|---|---|---|
| `WifiPolicy` | `src/wifi_policy.*` | time and facts; no Arduino at all | the radio, flash, HTTP |
| `WifiStore` | `src/wifi_store.*` | `Preferences` (ESP32) or `EEPROM` (ESP8266) | the radio, HTTP |
| `WifiLink` | `src/wifi_link.*` | `WiFi`, the policy, the store, the button | HTTP, the LED |
| `WifiPortal` | `src/wifi_portal.*` | `WifiLink`, `AsyncWebServer`, `DNSServer` | the LED, `WiFi` directly |
| `Blinker` | `src/blinker.*` | a `LedDriver`; no Arduino | the network, the server |
| `RgbLedDriver`, `GpioLedDriver` | `src/rgb_led_driver.h`, `src/gpio_led_driver.h` | FastLED / `digitalWrite` | rhythms |
| `Connectivity` | `src/connectivity.*` | all of the above; the facade | the bench routes |
| `BenchRoutes` | `src/bench_routes.*` | GPIO, I2C, the DUT's UART, `NtpServer` | the network |

Two of them - `WifiPolicy` and `Blinker` - are pure C++ and are therefore tested on a PC (`test/test_wifi_policy`, `test/test_blinker`), with a simulated radio and a fake LED that live in `test/`.

The facade is called `Connectivity` and not `Network` because the ESP32 core has its own `Network.h` and a global object named `Network`; on a case-insensitive file system a `network.h` of ours would be included in its place, and the build fails in the middle of the library.

## WiFi: modem sleep is off

The principles behind the whole network part, the ESP behaviour they account
for, the measurements and the known gaps: [wifi.md](wifi.md).

`setup()` calls `WiFi.setSleep(false)` right after the board joins. By default a
station dozes between the AP's beacons, so every reply waits for the next DTIM:
ping to the board swings from 6 ms to 260 ms, and every HTTP call a harness makes
pays that toll. Under a test bench that deliberately churns the air - turning its
own access point off and on, changing its channel, raising the DUT's own AP - a
dozing station also drops off the network and does not always come back quickly.
A harness then sees connect timeouts and `host is down` where the board is in
fact powered and fine.

Responsiveness is worth more here than current: METF is mains-powered, never a
battery. The ESP8266 core provides the same `setSleep(bool)` name for ESP32
compatibility, so one call covers both platforms.

## The server starts even when WiFi does not

`setup()` never waits for the network: it starts the radio, registers the routes
and calls `server.begin()`, and the first connect attempt happens five seconds
later, from `loop()`. Giving up in `setup()` instead - the early `return` this
firmware used to have - leaves the board in the worst state it can be in: the
board joins the network a minute later and answers pings, while the HTTP server,
never started, is gone until someone presses reset. One power cut that brings up
the board before the access point is enough to produce it, and nothing about the
board looks broken afterwards.

The earlier firmware also blocked for up to 15 seconds in
`WiFi.waitForConnectResult()` just to print the address at boot. It does not any
more: the address is printed when the board actually joins, and the console says
so again after every outage.

## What the board says about the network

`WifiLink::subscribe_events()` subscribes to the station events before the first `WiFi.begin()`,
so every loss of the network reaches the console: `wifi: disconnected, reason
201 NO_AP_FOUND` at the moment it happens, then one line a minute
(`WIFI_REPORT_PERIOD_MS`) with how long it has been offline and how many attempts
that took, and `wifi: back after N s and M attempts, ip ...` when it returns.

Without it the board is mute, and a METF that has lost the network looks exactly
like a METF that has hung: no answer on HTTP, nothing in the console. The
throttling is not cosmetic - attempts go every few seconds, so one line per event
would bury the console in a night.

Retrying does not stop, but its shape changes: after a couple of minutes without
the network the board also raises its own access point and then probes the router
once a minute. The full algorithm and its timings are in [wifi.md](wifi.md).

ESP32 has `WiFi.onEvent`; ESP8266 has `onStationModeDisconnected` /
`onStationModeGotIP`, whose subscriptions live only as long as the returned
`WiFiEventHandler`, hence the two handler members of `WifiLink`. The reason name (`NO_AP_FOUND`) comes from
`WiFi.disconnectReasonName()` and exists on ESP32 only; the number is printed on
both.

## Two serial ports on ESP32-C6

`main.cpp` defines `METF_SERIAL`, the port the DUT is read from:

- With `ARDUINO_USB_CDC_ON_BOOT=1` (the C6 env), `Serial` is the native USB CDC and carries METF's own logs; the DUT is read from `Serial0` (UART0 pins). Logs and DUT traffic are separate.
- On ESP8266 (and non-CDC ESP32), `METF_SERIAL` is `Serial`: logs and the DUT link share UART0, and `/serial` baud changes affect both.

On the C6, `setup()` waits 2 s before printing because the USB CDC comes up after the firmware starts and anything printed earlier is lost.

## Web server (`src/main.cpp`, `src/bench_routes.*`, `src/wifi_portal.*`)

`AsyncWebServer` on port 80, created in `main.cpp` and handed to whoever registers routes on it: `Connectivity` (the setup page, `/wifi`, `/rgb`, the captive-portal probes and `onNotFound`) and `BenchRoutes` (everything that drives the device under test). `loop()` calls `Connectivity::loop()` and `BenchRoutes::loop()`, and nothing else.

| Route | Notes |
|---|---|
| `GET /ping`, `GET /version` | connectivity; protocol version |
| `POST /pinMode`, `GET /digitalRead`, `POST /digitalWrite` | GPIO |
| `POST /pulse` | drive `pin` to `value` for `duration_ms`, then release to INPUT (high-Z); timed by the ESP on a `Ticker`, answered at once with `202`; 8 pins can pulse at once, `409` for a pin already pulsing |
| `POST /i2c` | `action=begin/setClock/setClockStretchLimit/ask/flush`; `ask` takes `address`, `hexstring`, `response` (bytes to read) and returns hex |
| `POST /serial` | `baudrate` (allow-listed in `kAllowedBauds`) and `flush=1`. A missing `baudrate` means 115200, so a flush-only call resets the speed |
| `GET /read`, `GET /read/stat` | drain the serial log; ring state as JSON (`lines`, `dropped`, `baud`, `capacity`, `line_len`, `bytes`) |
| `POST /ntp`, `GET /ntp/stat` | ESP32 only, see below |
| `POST /rgb` | the status LED, taken over by the bench; needs `RGB_DEFAULT_PIN` or `STATUS_LED_PIN` |
| `GET /`, `GET /wifi`, `POST /wifi` | the setup page and the network API; plus the captive-portal probes, which redirect a client of the board's own access point to `/` |

Parameters, responses and errors of every route: [api.md](api.md).

Conventions and traps:

- **Never block in a handler.** The handlers do not run on the loop thread - they run in the task that serves every connection of the board, and the library states it outright: *"You can not use yield or delay or any function that uses them inside the callbacks"*. The cost is not theoretical. A handler asleep for 4 seconds starves every other connection: AsyncTCP drops poll events once its queue passes three quarters (`CONFIG_ASYNC_TCP_QUEUE_SIZE`, 64), a client has 3 seconds to send its request (`setRxTimeout(3)` on accept) and unacknowledged data times out after 5 (`CONFIG_ASYNC_TCP_MAX_ACK_TIME`). That is how `/pulse` used to be written, and a bench that polls `/read` ten times a second saw read timeouts during every 4-second button press, then a board that stopped answering altogether by the end of an hour-long run.
- **Wait by arming a timer, and answer at once.** `/pulse` is the worked example: it arms a `Ticker` (in the core of both platforms - no extra library), answers `202` with the duration, and returns. On ESP8266 the timer uses `once_ms_scheduled()`, which runs the callback from `loop()` instead of SYS context; ESP32 has no such variant and its `Ticker` already dispatches from the `esp_timer` task.
- **Do not defer an answer to mark a moment in time.** Protocol 7 held the `/pulse` answer back with a chunked response whose filler returned `RESPONSE_TRY_AGAIN` until the timer fired, so that the answer would mean "line released". A deferred answer does not leave when it is ready: the filler is called again on the connection's next poll, roughly twice a second (`CONFIG_ASYNC_TCP_POLL_TIMER`). Measured: 240-336 ms late on a 20 ms pulse. `RESPONSE_TRY_AGAIN` is right for data that is not ready yet; it is wrong as a stopwatch.
- **Register `/x/sub` before `/x`.** `AsyncCallbackWebHandler::canHandle` also matches by prefix (`url.startsWith(_uri + "/")`), so `/read` or `/ntp` declared first would swallow `/read/stat` / `/ntp/stat`.
- POST parameters are form-encoded in the body: use `hasParam(name, true)` / `getParam(name, true)`. Without the second argument only the query string is searched - that silently broke `/serial` once; it now uses `param_any()` (body, then query).
- Errors: 400 via `http::send_400()` for missing/incorrect parameters, 500 for hardware failures (I2C errors, UDP bind failure), 404 for unknown routes. The helpers live in `src/http_util.*`.
- **Handlers do not touch the radio or the flash.** `POST /wifi` and `POST /rgb` validate, set an atomic flag and answer; `loop()` does the work. Writing NVS or calling `WiFi.begin()` from the server's task would block every other connection for as long as it takes, which is the same failure as a blocking `/pulse`.
- ESP8266 I2C stretch limit is `Wire.setClockStretchLimit(us)`; on ESP32 the same action maps to `Wire.setTimeOut(ms)` (µs / 1000, minimum 1000 ms).

## AsyncSerialBuffer (`src/AsyncSerialBuffer.*`)

Ring of fixed-size lines filled from `loop()` and drained by `/read`.

- Sized by one number, `ASB_BUFFER_BYTES`; `ASB_MAX_LINES = ASB_BUFFER_BYTES / ASB_MAX_LINE_LEN` unless `ASB_MAX_LINES` is set directly. One slot is always kept free, so usable capacity (`/read/stat` `capacity`) is `ASB_MAX_LINES - 1`. `ASB_MAX_LINE_LEN` includes the terminator.
- Defaults (6000 / 60) suit the ESP8266. `esp32-c6-super-mini` uses 65536 / 128 → 511 usable lines, so a full Waterius session fits without eviction.
- Longer lines are split into several buffer lines; the reader has to glue them back.
- When full, the oldest line is evicted and counted in `dropped()` (reset by `flush()`, exposed by `/read/stat`). Eviction is otherwise silent, and a silently shortened log makes tests green for the wrong reason - `dropped > 0` means the log has a hole.
- `LOCK()` / `UNLOCK()` defined here are the project's critical section: a FreeRTOS spinlock (`portENTER_CRITICAL(&mux)`) on ESP32, `noInterrupts()` on ESP8266. `main.cpp` reuses them for baud switching and `FastLED.show()`. On ESP32 they disable interrupts, so keep them short and don't take them for a single aligned word (see `dropped()`).

## NTP server (`src/NtpServer.*`, `src/ntp_packet.h`) - ESP32 only

UDP server on port 123 that answers the DUT with whatever time the test assigned. The board has no RTC and no internet: the moment is set over HTTP and time runs from `millis()`, so a bench works offline and a test can name a recognisable time.

- The listener does **not** start by itself (answering NTP on someone else's network unasked is a surprise); `action=start&epoch=<unix>` binds it.
- `action=time` moves the clock without touching the listener; `action=stop` closes the port (client gets ICMP port-unreachable); `action=drop&value=1` keeps the port but stays silent (client waits for a timeout, like an unreachable internet server).
- `stop` deletes the `AsyncUDP` object, and `begin` creates a new one. `AsyncUDP::close()` only drops the remote peer; the port binding and the handler go away only in the destructor (`udp_recv(NULL)` + `udp_remove`), so with one long-lived object a "stopped" server kept answering.
- `/ntp/stat` returns `running`, `dropping`, `epoch`, `requests`, `replies`, `dropped`, `ignored`, `last_epoch`, `last_client`.
- `AsyncUDP` delivers packets in the lwIP task, so counters are guarded by the server's own `portMUX_TYPE`, same pattern as AsyncSerialBuffer.
- The reply leaves through the pcb bound to 123: clients (the Waterius among them) accept an answer only when it comes from the NTP port.
- `ntp_packet.h` is the protocol, Arduino-free and host-tested. The request format and acceptance rules in the tests are copied from Waterius's `sync_time.cpp` / `core/timekeeping.cpp` (`parse_ntp_packet`): reply ≥ 48 bytes, leap indicator not `11`, transmit seconds after 1970.

ESP8266 has no AsyncUDP in its core, hence the `#ifdef ESP32`.

## Status LED (`src/blinker.*`, `src/*_led_driver.h`)

The onboard LED shows what the firmware is doing. Four colours and rhythms, and only four, so that they can be told apart across a room:

| Mode | LED |
|---|---|
| the five-second pause after power-up, and every connect attempt | blue, 1 s on / 1 s off |
| own access point up, waiting to be set up | blue, steady |
| on the network | green, dark for 100 ms every 3 s |
| network lost, reconnecting (the first two minutes) | red, 250 ms on / 250 ms off |
| hardware error: the access point did not start, or flash would not take a write | red, 1 s on / 1 s off |

Every settled mode moves, so a frozen picture means frozen firmware; the green heartbeat is there for exactly that. The rhythm is driven from `loop()`, so a blocked `loop()` shows up too.

`Blinker` holds the rhythms and knows nothing about the network - `Connectivity` translates `WifiLink`'s state into a colour and a pattern. The colour reaches the hardware through a `LedDriver`: `RgbLedDriver<PIN>` (WS2812B via FastLED, built with `-DRGB_DEFAULT_PIN=<pin>`; the pin is a template parameter because `FastLED.addLeds` takes it that way) or `GpioLedDriver` (a plain LED, built with `-DSTATUS_LED_PIN=<pin>`, any non-black colour means "lit"). A board with neither gets a driver that does nothing.

Status brightness is 24 of 255: the WS2812B on the SuperMini at full brightness is painful to look at.

`POST /rgb action=begin` takes the LED away from the status display and gives it to the bench, `action=status` gives it back, and so does a reboot. Colours set by the bench are stored, not written: `FastLED.show()` is called only from `loop()`, never from the server's task.

## Logging (`src/logging.h`)

`LOG_ERROR/INFO/DEBUG(a << b)` stream macros with an `HH:MM:SS:mmm` uptime prefix, compiled in by `-DLOG_LEVEL_ERROR|INFO|DEBUG`. They always write to `Serial` - see the serial-port section for what that means per platform.
