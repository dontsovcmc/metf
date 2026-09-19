# ESPTestFramework

Firmware that turns an ESP8266 or ESP32-C6 board into a test bench controlled over HTTP. Wire the board to the device you are testing, and your test scripts, in Python or anything else that speaks HTTP, can:

- drive and read its pins, including precisely timed button presses;
- talk to it over I2C;
- record its serial log and read it back;
- give it the time over NTP, with no internet needed (ESP32 only).

Supported boards: ESP32-C6 SuperMini (default build) and NodeMCU (ESP8266).

## Quick start

```bash
cp secrets.ini.template secrets.ini          # WiFi the board will join
pio run -e esp32-c6-super-mini -t upload --upload-port /dev/cu.usbmodemXXXX
pio device monitor --port /dev/cu.usbmodemXXXX   # the board prints its IP at boot
curl http://<ip>/version
```

The WiFi network is compiled into the firmware, so check `secrets.ini` before each upload. Details: [docs/build.md](docs/build.md). How the board behaves when that network is missing, and what it says about it: [docs/wifi.md](docs/wifi.md).

## URLs

Full reference with every parameter and response: [docs/api.md](docs/api.md).

| URL | Method | Purpose |
|---|---|---|
| [`/ping`](docs/api.md#get-ping) | GET | connectivity check, answers `pong` |
| [`/version`](docs/api.md#get-version) | GET | protocol version |
| [`/pinMode`](docs/api.md#post-pinmode) | POST | set pin mode |
| [`/digitalRead`](docs/api.md#get-digitalread) | GET | read a pin |
| [`/digitalWrite`](docs/api.md#post-digitalwrite) | POST | write a pin |
| [`/pulse`](docs/api.md#post-pulse) | POST | drive a pin for N ms, then release it; answers at once, the client times the wait |
| [`/i2c`](docs/api.md#post-i2c) | POST | I2C: `begin`, `setClock`, `setClockStretchLimit`, `ask`, `flush` |
| [`/serial`](docs/api.md#post-serial) | POST | DUT UART speed, clear the log |
| [`/read`](docs/api.md#get-read) | GET | take the recorded serial log |
| [`/read/stat`](docs/api.md#get-readstat) | GET | log buffer state, including lost lines |
| [`/ntp`](docs/api.md#post-ntp) | POST | NTP server: `start`, `time`, `stop`, `drop` (ESP32) |
| [`/ntp/stat`](docs/api.md#get-ntpstat) | GET | NTP server state and counters (ESP32) |
| [`/rgb`](docs/api.md#post-rgb) | POST | onboard WS2812B LED: `begin`, `brightness`, `color` (ESP32) |

## Examples

```bash
BOARD=192.168.1.50

# Is the board up, and which protocol does it speak?
curl http://$BOARD/version

# Press a button wired to GPIO 5: pull it low for 200 ms, then release
curl -d pin=5 -d value=0 -d duration_ms=200 http://$BOARD/pulse

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
