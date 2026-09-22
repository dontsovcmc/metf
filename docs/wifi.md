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
| METF firmware | protocol 10, this branch | this repository, relative links |
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

Since protocol 9 there is a third case, and it is what this part was rewritten
for: **the board was moved.** A bench carried to another room, another flat or
another office finds no network it knows, and until protocol 9 the only way out
was a USB cable, a checkout of this repository and a reflash. Now the board says
so on its LED, raises its own access point and takes a new network from a phone.

## Principles

### P1. The board never goes silent

Whatever happens to the network, the console says what happened. A disconnect is
printed with its reason the moment it occurs, a continuing outage is summarised
once a minute, and the return is printed with the address that came back.

Since protocol 9 the board also says it without a console: the LED shows the
mode (blue blinking - connecting, blue steady - own access point, green with a
beat - on the network, red - lost or broken), and `GET /wifi` answers the same
in JSON. All the LED modes blink except the steady blue of the access point, so on a board that is connecting, online or lost a frozen LED means frozen firmware.

*Enforced:* `WifiLink::note_down()` / `note_up()` ([`src/wifi_link.cpp`](../src/wifi_link.cpp)),
`Connectivity::show_status()` ([`src/connectivity.cpp`](../src/connectivity.cpp)),
`WifiPortal::on_status()` ([`src/wifi_portal.cpp`](../src/wifi_portal.cpp)).
*Checked:* every scenario in [Measurements](#measurements) shows the lines.

### P2. Booting does not depend on the network

`setup()` runs to the end and `server.begin()` is always reached, connected or
not. The server binds `IP_ANY` and needs no address to start
(`AsyncServer::begin`, [`AsyncTCP.cpp:1551`](https://github.com/mathieucarbou/AsyncTCP/blob/v3.3.2/src/AsyncTCP.cpp#L1551)).

Protocol 9 goes further: `setup()` does not wait for the network at all. The
first connect attempt happens five seconds after power-up, from `loop()`.

*Rationale:* the opposite - the early `return` this firmware had until protocol
7 - produces the exact failure P1 is about. The board joins the network, answers
pings and looks healthy, while the HTTP server was never started and will not
start until someone presses reset. One power cut that brings up the board before
the access point is enough.

*Enforced:* [`src/main.cpp`](../src/main.cpp), `setup()`.

### P3. Reconnection belongs to the firmware, and to one place in it

Until protocol 8 this principle said the opposite: reconnection was the core's
job and no loop was written here. That was right while the only question was
"when does the router come back". It is not enough for a board that may have
been moved: the core retries the one compiled-in network forever, with no
opinion about when to stop, when to raise an access point of its own, or when to
stop scanning because a human is setting the board up over that access point.

So the firmware takes it over, in one place and completely:

- `WiFi.setAutoReconnect(false)` and `WiFi.persistent(false)` are set before
  anything else, so the core neither retries behind the policy's back nor keeps
  a copy of the credentials in its own NVS.
- [`WifiPolicy`](../src/wifi_policy.h) decides what to do next and nothing else:
  it takes the time and a handful of facts and answers with one action. It has
  no Arduino in it, so all of its timings are tested on a PC
  ([`test/test_wifi_policy`](../test/test_wifi_policy), 28 scenarios).
- [`WifiLink`](../src/wifi_link.h) reports the facts and carries out the
  actions. It is the only place that touches the radio.

*What this buys, beside the access point:* the `AUTH_FAIL` hole is closed. The
core does not list `WIFI_REASON_AUTH_FAIL` among reconnectable reasons
([`STA.cpp:58-84`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/WiFi/src/STA.cpp#L58-L84)), so an access point answering with it used to leave the board
silently off the network forever. The policy does not care about the reason at
all: an attempt that does not produce an IP within its timeout is simply a
failed attempt.

### P4. An attempt has a deadline, and the deadline is ours

An attempt lasts `attempt_timeout_ms` (10 s) and then counts as failed, whatever
the core thinks. Two attempts make a round: the first one fast (on the saved
channel and BSSID), the second one with a full scan.

*Rationale:* the core's own notion of "failed" is not uniform and not fast.
Measured on this board: `waitForConnectResult()` returns in 2.9 s when the
network is absent and sits out all 60 s when the password is wrong. A policy
built on that would behave differently for each kind of failure, for no benefit:
what matters is only whether an IP arrived in time.

### P5. Nothing blocks the async server task

The HTTP handlers run in the task that serves every connection of the board, and
the library says so outright: *"This is fully asynchronous server and as such
does not run on the loop thread"*, *"You can not use yield or delay or any
function that uses them inside the callbacks"* ([`README.md`, "Important things
to remember"](https://github.com/mathieucarbou/ESPAsyncWebServer/blob/v3.3.15/README.md#L363-L364)). The ESP8266 build uses the older 1.2.3 package, whose README
carries the same sentence on line 138.

This is a WiFi principle, not a web-server one: a blocked task looks exactly
like a lost network from the outside - timeouts on every route.

Protocol 9 adds handlers that would be much worse offenders than the old
`/pulse`: `POST /wifi action=set` writes flash and re-associates, `action=scan`
takes seconds on the radio. None of them do any of that. They validate, set an
atomic flag and answer; `loop()` picks the flag up. The same rule covers the
LED: `FastLED.show()` is called only from `loop()`, never from a handler.

*Enforced:* `WifiLink::request_set()` and `apply_commands()`
([`src/wifi_link.cpp`](../src/wifi_link.cpp)), `Blinker::hold()`
([`src/blinker.cpp`](../src/blinker.cpp)).
*Cost of breaking it:* AsyncTCP discards poll events once its queue passes three
quarters of `CONFIG_ASYNC_TCP_QUEUE_SIZE` = 64 ([`AsyncTCP.cpp:205`](https://github.com/mathieucarbou/AsyncTCP/blob/v3.3.2/src/AsyncTCP.cpp#L205)); an accepted
client has 3 s to send its request ([`WebServer.cpp:56`](https://github.com/mathieucarbou/ESPAsyncWebServer/blob/v3.3.15/src/WebServer.cpp#L56)); unacknowledged data
times out after `CONFIG_ASYNC_TCP_MAX_ACK_TIME` = 5000 ms ([`AsyncTCP.h:76`](https://github.com/mathieucarbou/AsyncTCP/blob/v3.3.2/src/AsyncTCP.h#L76)).

### P6. Responsiveness beats power

`WiFi.setSleep(false)`, and again after every change of mode. A dozing station
answers on the access point's DTIM beacon, which turned ping to this board from
6 ms into 260 ms and made it drop off a deliberately churned network. METF is
never on batteries. See the "WiFi: modem sleep is off" section of
[architecture.md](architecture.md) for the measurement.

### P7. Retrying never stops - but a board nobody can reach must offer a way in

The board keeps trying for as long as it is powered. What changes with time is
the shape of the retries, not whether they happen:

- first two minutes of an outage, or the first round after power-up: attempts
  back to back, a round of two every few seconds;
- after that, the board also raises its own access point, and probes the router
  once a minute with a full scan;
- while somebody is connected to that access point and using the setup page, the
  probes pause: each one takes the radio away for seconds and would break the
  page the human is filling in. Ten minutes of silence from that client and the
  probes resume, because a phone that connected and went into a pocket must not
  hold the bench hostage.

*Rationale for probing rather than waiting:* the hour of silence this was first
designed with would have been a trap. Mains comes back, the board boots in two
seconds, the router takes one to two minutes - and the bench would be off the
network for an hour because of a race it loses every time. Measured below: the
board is back within a minute of the router returning.

*And a console that stays readable:* `note_down()` prints the first event in
full and then one summary a minute (`kReportPeriodMs`), carrying the outage
length and the attempt count.

### P8. The compiled network is a default, not a destiny

`secrets.ini` still decides where a freshly flashed board goes. But a network
set through the portal is saved in NVS (ESP32) or EEPROM (ESP8266) and overrides
it, and `POST /wifi action=forget` erases it and returns the board to the
compiled one.

The console says at boot which network is in force and where it came from
(`build` or `saved`), and prints the MAC beside it so the address can be pinned
by a DHCP reservation. `GET /wifi` answers the same over HTTP, minus the
password, which is never returned by anything.

*Why saved wins:* reflashing a board that is already in another room needs a
laptop and a cable in that room. Setting it up from a phone needs neither.

### P9. A failure is reported in words, not in a code

A board whose password was mistyped and a board whose router is switched off
look identical from the outside: the bench address goes quiet and an access
point appears. The person then guesses. Ending that guessing is the first job
of the setup page, and "причина 202" does not do it.

So every disconnect code is classified into one cause a human can act on
([`wifi_reason.h`](../src/wifi_reason.h)) - `password`, `not_found`,
`dropped`, `other` - and the page prints the phrase while `GET /wifi` carries
the key in `problem` and the raw code in `last_reason` beside it. The same
words now go to the console, which is where the ESP8266 gains them: the core's
own `disconnectReasonName()` exists only on the ESP32.

**The split is by who is speaking, not by the meaning of each code.** Below 200
the number comes from a deauth/disassoc frame the access point sent, and all it
proves is that the link died: "authentication expired" is sent both by a router
going down for a reboot and by one dropping a station that went quiet. Above
200 the number comes from the station's own connect machine, which knows what
it could not do - and those are trusted by name: 202 and 204 are the password,
201 with 210-212 is "no such network on the air" (210-212 are what a router
switched to WPA3-only or a signal below the threshold produce), 200 is a router
that went silent. The one exception below 200 is 15, the four-way handshake,
which is authoritative about the key.

*Why not decide each code on its merits:* because the guesses cost more than
they pay. Telling a person with the correct password that it is wrong makes
them retype and save it, and the next attempt gives the authoritative code
anyway. An unknown code is called `other`, not something confident.

*And the signal has to be clean for any of this to be true.* Before every
attempt the firmware tears the link down itself (`esp_wifi_disconnect()`), and
the core answers that with reason 8, the same code a router uses to say
goodbye. Such an echo is dropped, but **by time, not by a flag**: when the
board is already offline our own teardown produces no event at all, and a
flag armed for "the next event" then eats the real reason of every following
failure. That is not hypothetical - it is what the stand caught on the first
run: a board 137 s offline with ten failed attempts reporting `last_reason: 0`.
An event within 300 ms of our own command is an echo; a genuine failure takes
seconds. `last_reason` is also cleared when the board connects and when a new
network is set, so `problem` is about the current network's current trouble
and nothing else.

*What the classification cannot do:* tell a wrong password from a missing
network on a weak link. Measured on the stand against a router at -84 dBm: with
a deliberately wrong password the station gives up before the handshake and the
core reports 201 `NO_AP_FOUND`, so the page says "Сеть не найдена". The same
board against the bench router at -71 dBm reports 15 and says "Неверный
пароль". This is a property of the radio, not of the firmware - and the reason
the stand tests the typo against the near network.

## The algorithm

Timings, all in [`WifiPolicy::Config`](../src/wifi_policy.h):

| Name | Value | What it is |
|---|---|---|
| `start_delay_ms` | 5 s | from power-up to the first attempt |
| `attempt_timeout_ms` | 10 s | one attempt |
| `round_attempts` | 2 | attempts in a round: fast, then scan |
| `lost_pause_ms` | 5 s | between rounds after the network is lost |
| `lost_ap_after_ms` | 2 min | without the network before the access point goes up |
| `probe_period_ms` | 60 s | between probes while the access point is up |
| `portal_idle_ms` | 10 min | a silent client stops blocking the probes |
| `ap_stop_grace_ms` | 60 s | the access point stays this long after success if a client is on it |
| `ap_manual_ms` | 10 min | an access point raised by the button or by `action=ap` waits this long for somebody to arrive |
| `ap_retry_ms` | 10 s | `softAP()` failed - try again |
| `forget_fast_after` | 2 | failed fast attempts before the saved channel is erased |
| `radio_restart_every` | 4 | failed rounds before `WIFI_OFF` and back |

```
setup()
  ├─ console, UART to the DUT
  ├─ LED: blue, blinking
  ├─ WiFi.persistent(false), setAutoReconnect(false)   before anything else
  ├─ subscribe to the station events                   before the first begin()
  ├─ mode(STA), setSleep(false), read the saved network
  ├─ register routes: bench, setup page, /wifi, /rgb
  └─ server.begin()                                    always (P2)

loop(), every pass
  ├─ apply commands from HTTP (new network, forget, raise AP, scan)
  ├─ the button: held 3 s -> raise the access point
  ├─ facts from the hardware -> WifiPolicy::tick() -> one action
  │     Attempt(fast|scan) / StartAp / StopAp / RestartRadio / ForgetFast
  ├─ the access point follows the radio's channel (ESP32)
  ├─ LED: state -> colour and rhythm
  └─ the DUT's UART into the ring buffer

the policy, in words
  5 s -> round of 2 -> connected?  yes: online, save channel and BSSID
                                   no : own access point up
  online -> lost -> rounds every 5 s; after 2 min the access point goes up too
  access point up -> a probe with a full scan once a minute, unless a client
                     is using the setup page
  connected again -> the access point goes down (at once if nobody is on it,
                     a minute later if somebody is - they need to read the IP)
  the button or action=ap -> the access point goes up even on a board that is
                     on the network, and stays up while somebody is on it and
                     for ten minutes after the last sign of life
  new network from the portal -> saved, and a round starts immediately
```

**Fast connect and a router that moved.** After every successful connect the
board saves the channel and BSSID it used, and only if they changed. The first
attempt of a round uses them and skips the scan; the second one scans, so a
router that moved to another channel is found within one round. Two failed fast
attempts in a row and the pair is erased - a stale pair must not cost half of
every round forever.

**Its own access point.** Open, named `METF-XXXX` after the last two bytes of
the MAC, at most four clients, `192.168.4.1`. It comes up on the channel the
radio is already on, because one radio cannot hold the station and the access
point on two channels: the access point would keep its old channel in its config
and send no beacons at all, while `softAP()` returned true. On ESP32 the
firmware also moves the access point to follow the radio, after the mismatch has
held for 3 s and no more often than once in 10 s. On ESP8266 the SDK does it.

**The setup page** is plain HTML with no JavaScript, served at `/` on both
interfaces. A client of the board's own access point that asks for anything else
is redirected to it, and a DNS server answers every name with `192.168.4.1`, so
the phone opens the page by itself. The page refreshes itself while the board is
connecting and then shows the new IP address in large type - the bench addresses
the board by that address, and a human who has just moved the bench needs to
read it off the phone.

## ESP peculiarities accounted for

| # | Peculiarity | Where it bites | What the firmware does | Evidence |
|---|---|---|---|---|
| 1 | Async handlers run in the shared `async_tcp` task, and `delay()` there blocks every connection | a 4 s button press killed all HTTP for 4 s | `/pulse` arms a `Ticker`; `/wifi` and `/rgb` only set flags | [library README](https://github.com/mathieucarbou/ESPAsyncWebServer/blob/v3.3.15/README.md#L363-L364) |
| 2 | AsyncTCP drops poll events when its queue fills | silent loss of responses under a blocked task | keep the task free (P5) | [`AsyncTCP.cpp:205`](https://github.com/mathieucarbou/AsyncTCP/blob/v3.3.2/src/AsyncTCP.cpp#L205), queue 64 |
| 3 | An accepted client has 3 s to send its request | harness requests during a blocked task were refused, not queued | same | [`WebServer.cpp:56`](https://github.com/mathieucarbou/ESPAsyncWebServer/blob/v3.3.15/src/WebServer.cpp#L56) |
| 4 | Unacked data times out at 5 s | a 4 s block left 1 s of margin | same | [`AsyncTCP.h:76`](https://github.com/mathieucarbou/AsyncTCP/blob/v3.3.2/src/AsyncTCP.h#L76) |
| 5 | `tcp_poll` fires every ~500 ms (interval 1) | a deferred answer can lag the event it reports by up to half a second | `/pulse` answers at once; `/wifi` answers `202` and the client polls | [`AsyncTCP.cpp:68`](https://github.com/mathieucarbou/AsyncTCP/blob/v3.3.2/src/AsyncTCP.cpp#L68) |
| 6 | ESP8266 `Ticker::once_ms` runs the callback in SYS context | GPIO work from SYS context is a needless risk | ESP8266 uses `once_ms_scheduled()` | [`Ticker.h:136-148`](https://github.com/esp8266/Arduino/blob/3.1.2/libraries/Ticker/src/Ticker.h#L136-L148) |
| 7 | ESP32 `Ticker` dispatches from the `esp_timer` task | no SYS-context concern | `once_ms()` with a `std::function` | [`Ticker.cpp:38`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/Ticker/src/Ticker.cpp#L38) |
| 8 | The core's own retry policy is invisible and not uniform | a board that stops trying, or tries behind the policy's back | `setAutoReconnect(false)`; the policy owns every attempt (P3) | [`STA.cpp:231`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/WiFi/src/STA.cpp#L231), [`STA.cpp:150-165`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/WiFi/src/STA.cpp#L150-L165) |
| 9 | `AUTH_FAIL` is not in the core's reconnectable list | with a wrong password the core used to stop retrying for good | the policy retries on any failure, reason or no reason | [`STA.cpp:58-84`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/WiFi/src/STA.cpp#L58-L84) |
| 10 | `STAClass::disconnect()` returns early when the station is not connected | a connect attempt in progress would not be interrupted, and the next `begin()` could be refused | an attempt starts with `esp_wifi_disconnect()` directly | [`STA.cpp:527-545`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/WiFi/src/STA.cpp#L527-L545) |
| 11 | `WiFi.persistent(true)` is the default: every `begin()` writes the credentials into the core's own NVS | two copies of the network, one of them invisible to this firmware | `persistent(false)`; the only copy is `WifiStore`'s | [`WiFiGeneric.cpp:403`](https://github.com/espressif/arduino-esp32/blob/3.2.0/libraries/WiFi/src/WiFiGeneric.cpp#L403) |
| 12 | One radio cannot hold AP and STA on different channels: the AP keeps its configured channel and stops beaconing, while `softAP()` returned true | the setup page is unreachable exactly when it is needed | the AP starts on the radio's channel and follows it (3 s of mismatch, at most one move per 10 s) | `WifiLink::start_ap()`, `follow_channel()` |
| 13 | Modem sleep is on by default; replies wait for the DTIM beacon | ping 6 -> 260 ms, drops on a churned network | `WiFi.setSleep(false)`, re-applied after every mode change | [architecture.md](architecture.md) |
| 14 | `AsyncServer::begin()` binds `IP_ANY` and needs no address | is what makes P2 possible at all | server starts before any IP exists | [`AsyncTCP.cpp:1551`](https://github.com/mathieucarbou/AsyncTCP/blob/v3.3.2/src/AsyncTCP.cpp#L1551) |
| 15 | DHCP may hand out a different address after an outage | the harness addresses the board by IP | the address is pinned by MAC on the router; the address that came back is printed, shown on the page and returned by `GET /wifi` | `WifiLink::note_up()` |
| 16 | `millis()` wraps after 49 days | a board that lives longer than that | every comparison is an unsigned difference; a host test drives the policy across the wrap | `WifiPolicy::elapsed()`, `test_millis_wrap_during_outage` |
| 17 | ESP8266 event subscriptions die with the returned handler object | the ESP8266 build would log nothing | the handlers are members of `WifiLink` | `src/wifi_link.h` |
| 18 | ESP8266 has no atomic read-modify-write: `__atomic_exchange_1` does not link | the build fails at the link stage, not the compile | flags are taken with load+store; only `loop()` clears them | `WifiLink`, `Blinker`, function `take()` |
| 19 | The ESP32 core has its own `Network.h` and a global object named `Network` | on a case-insensitive file system our `network.h` is included in its place and the library fails to compile | the facade is `Connectivity` | `src/connectivity.h` |
| 20 | A scan takes the radio for about two seconds, and the core refuses one while the station is associating | the setup page would lose its client, or the scan would silently fail | scans are asynchronous, never started during an attempt, and the policy makes no attempts while a client is using the page | `WifiLink::poll_scan()`, `WifiPolicy::portal_busy()` |

## Measurements

On the bench ESP32-C6 SuperMini, 2026-09-22, firmware of this branch. Times are
seconds since reset, taken from the USB console. Builds with a deliberately
wrong network were made through `PLATFORMIO_BUILD_FLAGS`, as below.

**No access point** (build pointed at an SSID that does not exist):

```
  2.091  wifi: ssid NoSuchAP_metf_probe (build), fast ch 4, MAC ..., own AP METF-XXXX
  5.000  wifi: connecting to NoSuchAP_metf_probe, fast ch 4
  7.410  wifi: disconnected, reason 201 NO_AP_FOUND
 15.000  wifi: connecting to NoSuchAP_metf_probe, scan
 25.004  wifi: own access point METF-XXXX up, ch 1, open http://192.168.4.1/
 85.000  wifi: connecting to NoSuchAP_metf_probe, scan
 87.831  wifi: offline 80 s, 2 attempts, last reason 201
```

Exactly the designed ladder: first attempt at 5 s, second at 15 s, access point
at 25 s, then a probe every 60 s. No reset, no watchdog, no panic.

**The access point is really on the air.** With that build running, a scan from
a Mac standing next to the bench (`system_profiler SPAirPortDataType`) lists
`METF-XXXX`, `Channel: 1 (2GHz, 20MHz)`. This is worth checking rather than
assuming: `softAP()` returning true does not by itself mean beacons are going
out - see peculiarity 12.

**Fast connect against a full scan** (same board, real network, two boots):

```
 fast:  5.000 connecting, fast ch 4   ->  7.029 connected   2.03 s
 scan:  5.000 connecting, scan        ->  7.218 connected   2.22 s
```

On this router the saving is small, and that is the honest result: the win from
a saved channel grows with the number of channels the scan has to walk and with
how busy the air is. The reason the pair is kept is not only speed - it is that
the first attempt after a power cut does not depend on a scan at all.

**The status LED and responsiveness.** Driving the onboard WS2812B costs the
bench its answers if it is done the usual way. Measured the same way each time:
ten pulses of 2 s, `/ping` every 100 ms throughout, worst answer per run.

| LED driver | Runs with an answer over 500 ms | Worst |
|---|---|---|
| FastLED 3.10 (`FastLED.show()`) | 1 of 8 | 1014 ms |
| the LED frozen by `POST /rgb action=begin` | 0 of 8 | 33 ms |
| the core's `rgbLedWrite()` (re-inits RMT per write) | 1 of 10 | 584 ms |
| RMT initialised once, `rmtWriteAsync()` | 0 of 10 | 206 ms |

The median was 11-14 ms in every case: what the driver costs is not throughput
but rare, long stalls, exactly the kind `test_pulse.py` is there to catch - and
it did catch them, as an intermittent failure of the branch that `master` does
not show. The firmware now uses the last row, and FastLED is no longer a
dependency.

**Protocol and routes on a live board:** `pytest test/board --metf-host <ip>`,
25 tests, four runs in a row all green, including `test_pulse`, which pings the board throughout a
pulse and fails if any ping waits longer than a second. `GET /wifi` on the bench
board answers `state: online`, `mode: sta`, `fast: true`, `hw_error: false`.

**The portal, walked by a second board:** `pytest Utils/hil --stand -k portal`,
8 tests, all green - the access point seen in a scan from another board and on
the expected channel, the client counted by METF, the page and the captive
redirect served, and a full change of network through the form, after which the board
came back to the bench network at the same address with `source: saved`. The
stand is described in [Utils/hil/README.md](../Utils/hil/README.md).

**The troubles of a user, staged for real.** Until now the failure paths were
argued from the policy's host tests and from a made-up network name; nothing
had taken a real network away from a running board. They are now staged on a
managed WT32-ETH01 (`esp32_nat_router`, console over the wire) and on the bench
router, with a second board with ESP-AT firmware reading the setup page from
inside the board's own access point: `pytest Utils/hil --stand -k router`,
six scenarios, **all green in 10 min 39 s**.

| Staged | What the board did |
|---|---|
| the right network, the password mistyped | round of two attempts, then its own access point ~25 s after the change; `problem: password` from `last_reason` **15** (four-way handshake), the page says «Неверный пароль» |
| the password changed on the router | the board had been online, so the access point came up after the promised two minutes, not at once; the page named the password, the "phone" typed the new one into the form, and the board was back on that network within a minute |
| the network renamed | the old name gone means `problem: not_found` (201); the **new name appeared in the page's list of networks** - found by the board's own scan - and setting it from the page brought the board back |
| the router switched off, then on | own access point after two minutes; when the router returned the board rejoined **by itself**, with nothing touched - the once-a-minute probe doing its job |
| the router rebooted | survived it silently: back on the same network, access point never raised |
| the router moved to another channel | found the network on the new channel and stayed reachable there |

The three of those that end with the board back on the network are the ones
that matter most: they are the promise that a bench which lost its router does
not need a human at all, and they had never been shown before.

**The bench link is weak, and it shows.** RSSI on the bench sits at -71 to -80
dBm, and at that level `test/board` fails intermittently - about one test in
every other run. Measured rather than guessed: 300 `/ping` requests at 100 ms
gave a median of 11 ms with one or two answers of exactly 1015-1016 ms, which
is one TCP retransmission timeout and not a stall in the firmware (ICMP over
the same minute lost nothing, and a board that has been up for a while gives a
worst case of 252-265 ms). The console caught the honest version of the same
thing: a real `BEACON_TIMEOUT` at 20 s of uptime, three failed attempts, and
the board back on the network 37 s later without raising its access point -
which is exactly what P7 promises.

**Host tests:** `pio test -e native`, 58 test cases - 28 scenarios of the policy
on a virtual clock with a simulated radio, 12 of the LED rhythms, 10 of the NTP
packet, 8 of the disconnect-reason classification.

## Known gaps and non-goals

Listed so that an audit argues with a decision rather than discovering a hole.

1. **No real phone has opened the page.** Everything below it is covered: a
   second board with Espressif's ESP-AT firmware joins the access point over
   the air, walks the page, gets the captive redirect and sets a new network
   through the form (`Utils/hil`, eight tests, all green). What that cannot
   show is the part a phone does on its own - whether the captive window pops
   up, and how the page looks in the cut-down browser it opens. The sibling
   project (`esp32-opto`) has an open issue of exactly that kind, and the two
   suspects it names are avoided here: no `ON_AP_FILTER` on the handlers, and
   no JavaScript on the page.
2. **The access point is open.** Anyone within range can change the network of a
   bench board while it is looking for its router. The bench is on a private
   network and the exposure lasts only while the router is unreachable, but this
   is a deliberate trade, not an oversight.
3. **A dead link that still reports `WL_CONNECTED`.** The board believes it is
   online while the core says so and an IP is held. A router that silently stops
   forwarding leaves it believing that. `esp32-opto` pings the gateway to catch
   this; METF does not, because the harness talks to the board ten times a
   second and notices first.
4. **No reboot on a prolonged outage.** Deliberate: a board that reboots takes
   its serial ring buffer with it, and the buffer is often the only evidence of
   what the device under test did.
5. **ESP8266 is unverified.** The environment builds and the whole path -
   store in EEPROM, policy, access point, portal, status LED on GPIO 2 - is
   compiled and shares the code with the C6, but every measurement here is from
   the C6.
6. **The channel-follow and radio-restart paths are untested on hardware.**
   The station side of a channel move is now covered - the stand walks the
   router to another channel and the board follows (`Utils/hil/test_router.py`)
   - but `follow_channel()`, which moves the board's *own* access point after
   the radio, needs the board to be serving a phone while its router moves, and
   `RestartRadio` needs a radio that stops answering. The policy side of the
   restart is covered by a host test; the radio side is not.
7. **The board's own access point is not hidden from the neighbours while it
   probes.** It stays on the air between probes, so anyone in range sees a
   `METF-XXXX` network even in the seconds when the board is off talking to the
   router. Making it appear and disappear would be worse: a phone would lose the
   page mid-form.
8. **`forget` is reachable from the setup page without a confirmation.** One tap
   on "Сеть из прошивки" drops a saved network. The page is only reachable by
   somebody with physical proximity to the bench, and the network can be set
   again on the same page.

## Reproducing the checks

The bench board is flashed over USB, so every probe below is reversible by
reflashing the normal build.

```bash
# which port is the board: its USB serial number is its MAC
pio device list

# no access point: the board should raise its own at ~25 s
PLATFORMIO_BUILD_FLAGS='-D SSID_NAME=NoSuchAP_metf_probe' \
  pio run -e esp32-c6-super-mini -t upload
pio device monitor -e esp32-c6-super-mini

# is the access point on the air, and on which channel (macOS)
system_profiler SPAirPortDataType | grep -A3 METF-

# the setup page and the API, from the bench network
curl http://<board ip>/
curl -s http://<board ip>/wifi
curl -s -X POST http://<board ip>/wifi -d action=ap      # raise the AP now
curl -s -X POST http://<board ip>/wifi -d action=scan    # refresh the network list

# back to normal
pio run -e esp32-c6-super-mini -t upload
pio test -e native
pytest test/board --metf-host <board ip> -v
```

`SSID_NAME` and `SSID_PASS` are bare tokens, not quoted strings - the firmware
stringifies them with `VALUE()`. Passing `'-D SSID_NAME="\"name\""'` compiles
and flashes happily and then hunts for a network whose name includes the quote
characters; the boot banner is where that shows up.

Changing the router's channel needs a router of one's own. On this bench it is
the harness router (`Utils/hil/router.py` in the Waterius repository): change the
channel, and the board should fail its fast attempt and find the router with the
scan of the same round.
