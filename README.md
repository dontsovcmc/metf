# ESPTestFramework

Firmware that turns an ESP8266 or ESP32-C6 board into a test bench controlled over HTTP. Wire the board to the device you are testing, and your test scripts, in Python or anything else that speaks HTTP, can:

- drive and read its pins, including precisely timed button presses;
- talk to it over I2C;
- record its serial log and read it back;
- give it the time over NTP, with no internet needed (ESP32 only).

Everything above is served over HTTP by the board itself. Moved to another room? It raises its own access point when it cannot find the network, and a phone sets the new one from a web page - no reflashing.

Supported boards: ESP32-C6 SuperMini (default build) and NodeMCU (ESP8266).

## Quick start

```bash
cp secrets.ini.template secrets.ini          # WiFi the board will join
pio run -e esp32-c6-super-mini -t upload --upload-port /dev/cu.usbmodemXXXX
pio device monitor --port /dev/cu.usbmodemXXXX   # the board prints its IP at boot
curl http://<ip>/version
```

The network from `secrets.ini` is the default one. If the board cannot reach it, it raises its own open access point named `METF-XXXX` about 25 seconds after power-up: connect a phone to it, the setup page opens by itself, pick a network and the board saves it. A saved network overrides the compiled one until `POST /wifi action=forget`. Holding the BOOT button for 3 seconds raises the access point on demand.

Details: [docs/build.md](docs/build.md) for building and credentials, [docs/wifi.md](docs/wifi.md) for the network algorithm, its timings and what happens when the network disappears. The access point and the setup page are walked by a stand of their own - a second board plays the phone: [Utils/hil/README.md](Utils/hil/README.md).

## Web server and captive portal

The board is an asynchronous HTTP server, and it starts before the network does: the socket is bound at boot rather than after a connection, so the board answers the moment it has an address. Every URL below is served by it, and so is the setup page at `/`.

While the board's own access point is up, it also runs a DNS server that answers every name with its own address, and replies to the probes Android, iOS and Windows send to find out whether a network has internet. The phone concludes it has not and opens the setup page by itself - nothing to type. The page is plain HTML with no JavaScript, because the captive browser on iOS would not run it.

The page lists the networks the board can see, takes a password and shows the new address once the board is on the network. It is reachable from the ordinary network too, at `http://<ip>/`, so the network can be changed without leaving the bench.

## Status LED

The onboard LED shows what the firmware is doing. Four colours and rhythms, and only four, so that they can be told apart across a room:

| Mode | LED |
|---|---|
| the five-second pause after power-up, and every connect attempt | blue, 1 s on / 1 s off |
| own access point up, waiting to be set up | blue, steady |
| on the network | green, dark for 100 ms every 3 s |
| network lost, reconnecting (the first two minutes) | red, 250 ms on / 250 ms off |
| hardware error: the access point did not start, or flash would not take a write | red, 1 s on / 1 s off |

All of them blink except the steady blue of the access point. So on a board that is connecting, online or lost, a frozen picture means frozen firmware - the green heartbeat exists for exactly that. The rhythm is driven from the main loop, so a blocked loop shows up too.

`POST /rgb action=begin` takes the LED away from this display and gives it to the bench; `action=status` gives it back, and so does a reboot. On a board with a plain LED instead of an RGB one the same rhythms are shown without colour. How it is wired and why it is driven the way it is: [docs/architecture.md](docs/architecture.md#status-led).

## URLs

Full reference with every parameter and response: [docs/api.md](docs/api.md).

| URL | Method | Purpose |
|---|---|---|
| [`/ping`](docs/api.md#get-ping) | GET | connectivity check, answers `pong` |
| [`/version`](docs/api.md#get-version) | GET | protocol version |
| every answer | — | header [`X-Uptime-Ms`](docs/api.md#x-uptime-ms): ms since the board booted; a smaller value than before means it restarted |
| [`/pinMode`](docs/api.md#post-pinmode) | POST | set pin mode |
| [`/digitalRead`](docs/api.md#get-digitalread) | GET | read a pin |
| [`/digitalWrite`](docs/api.md#post-digitalwrite) | POST | write a pin |
| [`/pulse`](docs/api.md#post-pulse) | POST | a waveform: pins, levels and segment durations in one JSON body, or one level for N ms as a form; answers at once with the batch length |
| [`/pulse/stat`](docs/api.md#get-pulsestat) | GET | what the last batch really did: every edge by the board's clock |
| [`/i2c`](docs/api.md#post-i2c) | POST | I2C: `begin`, `setClock`, `setClockStretchLimit`, `ask`, `flush` |
| [`/serial`](docs/api.md#post-serial) | POST | DUT UART speed, clear the log |
| [`/read`](docs/api.md#get-read) | GET | take the recorded serial log; with `ack=<n>` the board keeps the lines until the reader confirms them |
| [`/read/stat`](docs/api.md#get-readstat) | GET | log buffer state, including lost lines |
| [`/ntp`](docs/api.md#post-ntp) | POST | NTP server: `start`, `time`, `stop`, `drop` (ESP32) |
| [`/ntp/stat`](docs/api.md#get-ntpstat) | GET | NTP server state and counters (ESP32) |
| [`/rgb`](docs/api.md#post-rgb) | POST | onboard LED: `begin`, `brightness`, `color`, `status` |
| [`/wifi`](docs/api.md#get-wifi) | GET | network state: mode, address, signal, outage |
| [`/wifi`](docs/api.md#post-wifi) | POST | network: `set`, `forget`, `ap`, `scan` |
| [`/`](docs/api.md#get-) | GET | setup page: pick a network and enter its password |

## Examples

```bash
BOARD=192.168.1.50

# Is the board up, and which protocol does it speak?
curl http://$BOARD/version

# Press a button wired to GPIO 5: pull it low for 200 ms, then release
curl -d pin=5 -d value=0 -d duration_ms=200 http://$BOARD/pulse
# two closures 800 ms apart, and a 1 ms pulse on a neighbour 100 ms in
curl -H 'Content-Type: application/json' -d '{"lines":[
  {"pin": 5, "value": 0, "edges": [300, 800, 300]},
  {"pin": 6, "value": 0, "at_ms": 100, "edges": [1]}]}' http://$BOARD/pulse
curl http://$BOARD/pulse/stat

# Read a pin
curl "http://$BOARD/digitalRead?pin=4"

# I2C: send one byte 0x41 to slave 18 and read 3 bytes back
curl -d action=begin http://$BOARD/i2c
curl -d action=ask -d address=18 -d hexstring=41 -d response=3 http://$BOARD/i2c

# Serial log: listen at 9600 from a clean buffer, run the test, then collect
curl -d baudrate=9600 -d flush=1 http://$BOARD/serial
curl http://$BOARD/read/stat          # "dropped" must be 0, or the log has a hole
curl http://$BOARD/read

# Give the device 2026-01-01 00:00:00 UTC over NTP, check it asked
curl -d action=start -d epoch=1767225600 http://$BOARD/ntp
curl http://$BOARD/ntp/stat
```

`test/board/conftest.py` has a minimal Python client (`Board.get`, `Board.post`) that uses only the standard library.

## Documentation

- [docs/api.md](docs/api.md) - every URL, its parameters and responses
- [docs/build.md](docs/build.md) - building, flashing, WiFi, build flags
- [docs/testing.md](docs/testing.md) - host tests, on-board tests, live-board pytest suite
- [docs/architecture.md](docs/architecture.md) - how the firmware works inside

## License

[MIT](LICENSE)
