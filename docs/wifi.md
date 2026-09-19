# WiFi on the board: principles, algorithm, and the ESP behaviour it accounts for

This document exists to be audited. It states what the firmware promises about
the network, how it delivers that, which peculiarities of the Espressif stack
the design leans on, and what is deliberately left undone. Every claim about
somebody else's code carries a file and a line; every number carries the method
that produced it.

Read [architecture.md](architecture.md) for the rest of the firmware; this file
covers only the network and its supervision.

## What is under audit

| Component | Version | Where the links point |
|---|---|---|
| METF firmware | protocol 7, this branch | this repository, relative links |
| ESP32 Arduino core | 3.2.0, pinned by `platform-espressif32` 54.03.20 in `platformio.ini` | [espressif/arduino-esp32 @ 3.2.0](https://github.com/espressif/arduino-esp32/tree/3.2.0) |
| ESP8266 Arduino core | 3.1.2 (`espressif8266@4.2.1`) | [esp8266/Arduino @ 3.1.2](https://github.com/esp8266/Arduino/tree/3.1.2) |
| AsyncTCP (ESP32) | 3.3.2 | [mathieucarbou/AsyncTCP @ v3.3.2](https://github.com/mathieucarbou/AsyncTCP/tree/v3.3.2) |
| ESPAsyncWebServer (ESP32) | 3.3.15 | [mathieucarbou/ESPAsyncWebServer @ v3.3.15](https://github.com/mathieucarbou/ESPAsyncWebServer/tree/v3.3.15) |
| ESP Async WebServer (ESP8266) | 1.2.3, PlatformIO registry | installed copy under `.pio/libdeps/nodemcuv2/` |

The board on the bench is an ESP32-C6 SuperMini. The ESP8266 environment builds
and is kept working, but nothing in this document was measured on it.

### How to read the references

Every reference to somebody else's code is a permalink to the exact version this
firmware pins, so its line numbers do not drift. They were checked, not assumed:
each cited file was compared with the copy PlatformIO installed here, and all of
them are byte-identical.

```bash
CORE=~/.platformio/packages/framework-arduinoespressif32@src-*/libraries/WiFi/src
curl -s https://raw.githubusercontent.com/espressif/arduino-esp32/3.2.0/libraries/WiFi/src/STA.cpp | shasum -a 256
shasum -a 256 $CORE/STA.cpp
```

One reference cannot be a permalink: the ESP8266 web server comes from the
PlatformIO registry (`ESP Async WebServer@1.2.3`), whose tarball does not match
any tagged commit of the upstream repository, so its README is cited by line
inside the installed package. The identical sentence in the ESP32 fork is linked
instead.

References into this repository are relative links. Line numbers there move with
the code, so the name of the function or constant is always given beside them -
that is what to search for if a link lands a few lines off.

## What the board is, and what "working WiFi" has to mean for it

METF is the manipulator of a hardware test bench: it presses the button of the
device under test, drives its counter inputs, reads its UART and answers HTTP
from the harness. It is mains-powered and permanently on. The harness addresses
it by a fixed IP and talks to it ten times a second for hours.

Two failure modes matter, and they are not symmetric:

- **Unreachable** costs a test run. Bad, recoverable, obvious.
- **Unreachable and indistinguishable from broken** costs an investigation. The
  harness sees connect timeouts; a human sees a board with a lit LED. Whether
  the network went away, the firmware hung, or the board is dying is unknown,
  and finding out takes hours.

Everything below follows from treating the second as the one to design against.

## Principles

### P1. The board never goes silent

Whatever happens to the network, the console says what happened. A disconnect is
printed with its reason the moment it occurs, a continuing outage is summarised
once a minute, and the return is printed with the address that came back.

*Enforced:* `wifi_watch()` ([`src/main.cpp:95`](../src/main.cpp#L95)), `wifi_note_down()`
([`src/main.cpp:65`](../src/main.cpp#L65)), `wifi_note_up()` ([`src/main.cpp:80`](../src/main.cpp#L80)).
*Checked:* every scenario in [Measurements](#measurements) shows the lines.

### P2. Booting does not depend on the network

`setup()` runs to the end and `server.begin()` is always reached, connected or
not. The server binds `IP_ANY` and needs no address to start
(`AsyncServer::begin`, [`AsyncTCP.cpp:1551`](https://github.com/mathieucarbou/AsyncTCP/blob/v3.3.2/src/AsyncTCP.cpp#L1551)).

*Rationale:* the opposite - the early `return` this firmware had until protocol
7 - produces the exact failure P1 is about. The core keeps reconnecting, so the
board joins the network, answers pings and looks healthy, while the HTTP server
was never started and will not start until someone presses reset. One power cut
that brings up the board before the access point is enough.

*Enforced:* [`src/main.cpp:275`](../src/main.cpp#L275) (log and continue), [`src/main.cpp:826`](../src/main.cpp#L826).

### P3. Reconnection is the core's job, not the firmware's

No reconnect loop is written here. The Arduino core reconnects on its own for
every reason worth retrying, and a hand-written loop on top would race with it.

*Evidence:* `_autoReconnect(true)` by default ([`STA.cpp:231`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/WiFi/src/STA.cpp#L231)); on
`ARDUINO_EVENT_WIFI_STA_DISCONNECTED` the core calls `disconnect()` then
`connect()` when the reason is reconnectable ([`STA.cpp:150-165`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/WiFi/src/STA.cpp#L150-L165)); the list of
such reasons includes `NO_AP_FOUND`, `BEACON_TIMEOUT`, `AUTH_EXPIRE` and
`4WAY_HANDSHAKE_TIMEOUT` ([`STA.cpp:58-84`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/WiFi/src/STA.cpp#L58-L84)).

*Limits, and they are real:* `WIFI_REASON_ASSOC_LEAVE` is excluded - a voluntary
`WiFi.disconnect()` is never followed by a reconnect ([`STA.cpp:151`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/WiFi/src/STA.cpp#L151)), which is
why the `disconnect()` in `setup()` is immediately followed by `begin()`. And
`WIFI_REASON_AUTH_FAIL` is *not* in the reconnectable list: see
[Known gaps](#known-gaps-and-non-goals).

### P4. Waiting at boot buys one log line, and is priced accordingly

`WiFi.waitForConnectResult(WIFI_CONNECT_WAIT_MS)` waits 15 s
(`src/main.cpp:58,275`), not the core's default of 60 s
([`WiFiSTA.h:68`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/WiFi/src/WiFiSTA.h#L68)). Since the server starts either way (P2), the only thing the
wait buys is the address printed in the boot banner.

*Rationale:* the default is not a uniform minute. The core gives up in 2.9 s
when the network is absent and sits out all 60 s when the password is wrong -
both measured below. That minute is a minute with no HTTP on the board.

### P5. Nothing blocks the async server task

The HTTP handlers run in the task that serves every connection of the board, and
the library says so outright: *"This is fully asynchronous server and as such
does not run on the loop thread"*, *"You can not use yield or delay or any
function that uses them inside the callbacks"* ([`README.md`, "Important things
to remember"](https://github.com/mathieucarbou/ESPAsyncWebServer/blob/v3.3.15/README.md#L363-L364)). The ESP8266 build uses the older 1.2.3 package, whose README
carries the same sentence on line 138.

This is a WiFi principle, not a web-server one: a blocked task looks exactly
like a lost network from the outside - timeouts on every route - and it was the
real cause of a bench failure before protocol 7. `/pulse` used to hold the task
for the whole 4-second button press.

*Enforced:* `/pulse` arms a `Ticker` and returns; the answer leaves through a
chunked response that reports `RESPONSE_TRY_AGAIN` until the pulse ends
([`src/main.cpp:399-401`](../src/main.cpp#L399-L401) and the handler above it).
*Cost of breaking it:* AsyncTCP discards poll events once its queue passes three
quarters of `CONFIG_ASYNC_TCP_QUEUE_SIZE` = 64 ([`AsyncTCP.cpp:205`](https://github.com/mathieucarbou/AsyncTCP/blob/v3.3.2/src/AsyncTCP.cpp#L205), with the
library's own comment about callbacks "starving other connections"); an accepted
client has 3 s to send its request ([`WebServer.cpp:56`](https://github.com/mathieucarbou/ESPAsyncWebServer/blob/v3.3.15/src/WebServer.cpp#L56)); unacknowledged data
times out after `CONFIG_ASYNC_TCP_MAX_ACK_TIME` = 5000 ms ([`AsyncTCP.h:76`](https://github.com/mathieucarbou/AsyncTCP/blob/v3.3.2/src/AsyncTCP.h#L76)).

### P6. Responsiveness beats power

`WiFi.setSleep(false)` ([`src/main.cpp:287`](../src/main.cpp#L287)). A dozing station answers on the
access point's DTIM beacon, which turned ping to this board from 6 ms into
260 ms and made it drop off a deliberately churned network. METF is never on
batteries. See the "WiFi: modem sleep is off" section of
[architecture.md](architecture.md) for the measurement.

### P7. Retrying forever is correct here - and must stay readable

The core retries every 2.4 s while the access point is missing and every 3.1 s
while the password is wrong, indefinitely, with no backoff (measured below).
That is the wanted behaviour: the router is expected back, and the board must
rejoin without a human.

What is not wanted is a console full of it. `wifi_note_down()` prints the first
event in full and then one summary a minute (`WIFI_REPORT_PERIOD_MS`,
[`src/main.cpp:53`](../src/main.cpp#L53)), carrying the outage length and the attempt count so the
throttling loses nothing an operator needs.

*Contrast worth keeping in mind:* the harness's own AT board had to be stopped
from doing this, because it was hunting an access point the harness had switched
off on purpose. Forever is right when the network is expected to return, and
wrong when it is not. METF is the first case.

### P8. The network is fixed at build time and visible at boot

Credentials are compiled in from `secrets.ini` (`SSID_NAME` / `SSID_PASS`), so
reflashing with a different file silently moves the board to another network.
The boot banner prints the SSID - the only place the compiled-in network is
visible - and the MAC, so the address can be pinned by a DHCP reservation
(`src/main.cpp`, the banner after the connect attempt). The harness addresses the
board by that reserved IP.

## The algorithm

```
setup()
  ├─ console, UART to the DUT
  ├─ wifi_watch()                     subscribe BEFORE the first begin(),
  │                                   or the first failures are invisible
  ├─ WiFi.mode(STA); disconnect(); begin(ssid, pass)
  ├─ waitForConnectResult(15 s)
  │     └─ not connected → log "starting the server anyway", continue (P2)
  ├─ WiFi.setSleep(false)             applies to the interface, survives the
  │                                   core's internal disconnect()/connect()
  ├─ register routes
  └─ server.begin()                   always

core, asynchronously, for as long as the board is powered
  ├─ STA_DISCONNECTED → wifi_note_down(reason)
  │     └─ core decides on its own whether to retry (P3)
  └─ STA_GOT_IP       → wifi_note_up()  prints only if it had been down

loop()
  └─ drain the DUT's UART into the ring buffer. Nothing about WiFi.
```

Both event subscriptions are registered in `wifi_watch()` ([`src/main.cpp:95`](../src/main.cpp#L95)):
`WiFi.onEvent()` on ESP32, `onStationModeDisconnected` / `onStationModeGotIP` on
ESP8266, whose subscriptions live only as long as the returned
`WiFiEventHandler` - hence the two globals above the function.

## ESP peculiarities accounted for

| # | Peculiarity | Where it bites | What the firmware does | Evidence |
|---|---|---|---|---|
| 1 | Async handlers run in the shared `async_tcp` task, and `delay()` there blocks every connection | a 4 s button press killed all HTTP for 4 s | `/pulse` arms a `Ticker`, answers via `RESPONSE_TRY_AGAIN` | [library README](https://github.com/mathieucarbou/ESPAsyncWebServer/blob/v3.3.15/README.md#L363-L364); [`src/main.cpp:399`](../src/main.cpp#L399) |
| 2 | AsyncTCP drops poll events when its queue fills | silent loss of responses under a blocked task | keep the task free (P5) | [`AsyncTCP.cpp:205`](https://github.com/mathieucarbou/AsyncTCP/blob/v3.3.2/src/AsyncTCP.cpp#L205), queue 64 |
| 3 | An accepted client has 3 s to send its request | harness requests during a blocked task were refused, not queued | same | [`WebServer.cpp:56`](https://github.com/mathieucarbou/ESPAsyncWebServer/blob/v3.3.15/src/WebServer.cpp#L56) |
| 4 | Unacked data times out at 5 s | a 4 s block left 1 s of margin | same | [`AsyncTCP.h:76`](https://github.com/mathieucarbou/AsyncTCP/blob/v3.3.2/src/AsyncTCP.h#L76) |
| 5 | `tcp_poll` fires every ~500 ms (interval 1) | a deferred answer can lag the event it reports by up to half a second | documented as a protocol property of `/pulse` | [`AsyncTCP.cpp:68`](https://github.com/mathieucarbou/AsyncTCP/blob/v3.3.2/src/AsyncTCP.cpp#L68); [api.md](api.md) |
| 6 | ESP8266 `Ticker::once_ms` runs the callback in SYS context | GPIO work from SYS context is a needless risk | ESP8266 uses `once_ms_scheduled()`, which runs it from `loop()` | [`Ticker.h:136-148`](https://github.com/esp8266/Arduino/blob/3.1.2/libraries/Ticker/src/Ticker.h#L136-L148), the comments above both methods; [`src/main.cpp:399`](../src/main.cpp#L399) |
| 7 | ESP32 `Ticker` dispatches from the `esp_timer` task | no SYS-context concern, no scheduled variant needed | `once_ms()` | [`Ticker.cpp:38`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/Ticker/src/Ticker.cpp#L38), `dispatch_method = ESP_TIMER_TASK` |
| 8 | `waitForConnectResult()` defaults to 60 s and is not uniform: 2.9 s with no AP, the full 60 s with a wrong password | a minute of no HTTP after a bad flash | 15 s, and the server starts regardless | [`WiFiSTA.h:68`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/WiFi/src/WiFiSTA.h#L68); [`src/main.cpp:58`](../src/main.cpp#L58) |
| 9 | The core reconnects by itself for reconnectable reasons | a hand-written retry loop would race with it | no such loop exists | [`STA.cpp:231`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/WiFi/src/STA.cpp#L231), [`STA.cpp:150-165`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/WiFi/src/STA.cpp#L150-L165), [`STA.cpp:58-84`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/WiFi/src/STA.cpp#L58-L84) |
| 10 | `ASSOC_LEAVE` - a voluntary `disconnect()` - is never auto-reconnected | a stray `WiFi.disconnect()` would park the board forever | the only `disconnect()` is immediately followed by `begin()` | [`STA.cpp:151`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/WiFi/src/STA.cpp#L151); [`src/main.cpp:272-274`](../src/main.cpp#L272-L274) |
| 11 | `AUTH_FAIL` is not in the reconnectable list | with a wrong password some APs make the core stop retrying | not handled - see [Known gaps](#known-gaps-and-non-goals) | [`STA.cpp:58-84`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/WiFi/src/STA.cpp#L58-L84), [`STA.cpp:140`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/WiFi/src/STA.cpp#L140) |
| 12 | Modem sleep is on by default; replies wait for the DTIM beacon | ping 6 → 260 ms, drops on a churned network | `WiFi.setSleep(false)` | [`src/main.cpp:287`](../src/main.cpp#L287); [architecture.md](architecture.md) |
| 13 | `AsyncServer::begin()` binds `IP_ANY` and needs no address | is what makes P2 possible at all | server starts before any IP exists | [`AsyncTCP.cpp:1551`](https://github.com/mathieucarbou/AsyncTCP/blob/v3.3.2/src/AsyncTCP.cpp#L1551) |
| 14 | DHCP may hand out a different address after an outage | the harness addresses the board by IP | the address is pinned by MAC on the router, and the address that came back is printed on return | `wifi_note_up()`, [`src/main.cpp:80`](../src/main.cpp#L80) |
| 15 | `millis()` wraps after 49 days | a board that lives longer than that | the throttle compares unsigned differences, never absolute values | [`src/main.cpp:65-79`](../src/main.cpp#L65-L79) |
| 16 | ESP8266 event subscriptions die with the returned handler object | the ESP8266 build would log nothing | the handlers are globals | [`src/main.cpp:89-92`](../src/main.cpp#L89-L92) |
| 17 | Credentials are compile-time | a reflash moves the board to another network with no error to see | the SSID is printed at boot, the MAC beside it | `src/main.cpp`, boot banner; [build.md](build.md) |

## Measurements

All on the bench ESP32-C6 (`E4:B0:63:41:F4:E4`), 2026-09-19, firmware of this
branch, temporary builds with credentials overridden through
`PLATFORMIO_BUILD_FLAGS`. Times are seconds since reset, taken from the USB
console.

**No access point** (build pointed at an SSID that does not exist):

```
  2.2  Connect to wi-fi ssid: NoSuchAP_metf_probe
  4.9  wifi: disconnected, reason 201 NO_AP_FOUND
  5.1  WiFi not connected: starting the server, the core keeps trying
 65.0  wifi: offline 60 s, 26 attempts, last reason 201
125.4  wifi: offline 120 s, 51 attempts, last reason 201
```

`waitForConnectResult()` returned in 2.9 s. Retries every 2.408 s, evenly, with
no backoff and no end - 101 attempts over the 240 s of the longest run. No
reset, no watchdog, no panic.

**Wrong password** (real SSID, password overridden):

```
  2.2  Connect to wi-fi ssid: <real>
  5.5  wifi: disconnected, reason 15
      ... every 3.07 s ...
 62.7  WiFi not connected: starting the server, the core keeps trying
```

Here `waitForConnectResult()` used its whole timeout. The access point answered
`4WAY_HANDSHAKE_TIMEOUT`, which is reconnectable, so the retries continued
indefinitely, again with no reset.

**The access point disappears and comes back** (board joined to the bench's own
lab AP; `ap disable`, then `ap enable` on the router):

```
  4.9  IP Address: 192.168.4.2 ...
 28.3  wifi: disconnected, reason 2 AUTH_EXPIRE
 59.5  wifi: back after 31 s and 13 attempts, ip 192.168.4.2
```

The board rejoined by itself after 31 s and 13 attempts, with the same address,
and said so in one line. This is the scenario the design is for.

**The server stays responsive during a pulse** (the P5 check, protocol 7):

```
pulse 4000 ms: answer after 4350 ms
during it: 50 /ping requests, median 17 ms, max 173 ms
```

## Known gaps and non-goals

Listed so that an audit argues with a decision rather than discovering a hole.

1. **`AUTH_FAIL` stops the retries.** [`STA.cpp:58-84`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/WiFi/src/STA.cpp#L58-L84) does not list
   `WIFI_REASON_AUTH_FAIL`, so an access point that answers with it - rather
   than the `4WAY_HANDSHAKE_TIMEOUT` measured here - leaves the core with
   `DoReconnect == false` after its one `first_connect` retry. The board would
   then print one disconnect line and stay off the network silently, which is
   exactly what P1 is meant to prevent. Reaching this state requires flashing a
   wrong password, which is noticed immediately, but the failure mode is the
   ugly kind. A fix would be to re-arm `WiFi.begin()` from the firmware when a
   disconnect reason is known not to be retried by the core.
2. **No health endpoint.** The network state is visible on the USB console only.
   The harness cannot ask the board how long it has been up, how many times it
   lost the network, or at what RSSI it is sitting - and a bench that logs its
   own flakiness is worth more than one that hides it. A `GET /wifi` returning
   RSSI, uptime, outage count and the last reason would close it.
3. **No reboot on a prolonged outage.** Deliberate: a board that reboots takes
   its serial ring buffer with it, and the buffer is often the only evidence of
   what the device under test did.
4. **ESP8266 is unverified.** The environment builds and the event handlers are
   written, but every measurement here is from the C6. The ESP8266 reconnect
   policy lives in the closed NONOS SDK (`wifi_station_set_reconnect_policy`,
   [`ESP8266WiFiSTA.cpp:443`](https://github.com/esp8266/Arduino/blob/3.1.2/libraries/ESP8266WiFi/src/ESP8266WiFiSTA.cpp#L443)) and its default was not confirmed from source.
5. **No AP fallback, no static-IP fallback, no captive portal.** The board is
   configured at build time on purpose (P8); a fallback mode would be one more
   state to reason about on a bench whose network is under the harness's control.

## Reproducing the checks

The bench board is flashed over USB, so every probe below is reversible by
reflashing the normal build.

```bash
# which port is the board: its USB serial number is its MAC
system_profiler SPUSBDataType | grep -A4 "USB JTAG"
arp -n <board ip>

# no access point
PLATFORMIO_BUILD_FLAGS='-D SSID_NAME=NoSuchAP_metf_probe' \
  pio run -e esp32-c6-super-mini -t upload --upload-port /dev/cu.usbmodemXXXX

# wrong password, real network
PLATFORMIO_BUILD_FLAGS='-D SSID_PASS=wrong-password-probe' \
  pio run -e esp32-c6-super-mini -t upload --upload-port /dev/cu.usbmodemXXXX

# back to normal
pio run -e esp32-c6-super-mini -t upload --upload-port /dev/cu.usbmodemXXXX
pytest test/board --metf-host <board ip> -v
```

`SSID_NAME` and `SSID_PASS` are bare tokens, not quoted strings - the firmware
stringifies them with `VALUE()`. Passing `'-D SSID_NAME="\"name\""'` compiles
and flashes happily and then hunts for a network whose name includes the quote
characters; the boot banner is where that shows up.

The disappearing-access-point check needs an access point of one's own. On this
bench it is the harness router (`Utils/hil/router.py ap off` / `ap on` in the
Waterius repository, console over the wire so the command cannot cut itself off).

The pulse check is automated: `test/board/test_pulse.py` pings the board
throughout a 2-second pulse and fails if any ping waits longer than a second.
