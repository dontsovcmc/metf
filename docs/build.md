# Build, flash, configure

`pio` may not be on the shell PATH; the PlatformIO install lives at `~/.platformio/penv/bin/pio`.

## Build and upload

```bash
pio run                                   # default env: esp32-c6-super-mini
pio run -e nodemcuv2
pio run -e esp32-c6-super-mini -t upload --upload-port /dev/cu.usbmodemXXXX
pio device monitor -e esp32-c6-super-mini --port /dev/cu.usbmodemXXXX
```

The upload port belongs on the command line, not in `platformio.ini` - the value
there goes stale as soon as the board is replugged. `ls /dev/cu.*` finds it: the
ESP32-C6 SuperMini is a native USB CDC port (`usbmodem*`), a board behind a
USB-UART bridge is `usbserial-*`.

## Environments

- **ESP8266** (`nodemcuv2`): `espressif8266@4.2.1`, `ESPAsyncTCP`, ESP Async WebServer 1.2.3.
- **ESP32-C6** (`esp32-c6-super-mini`, default): pioarduino `platform-espressif32`, `AsyncTCP`, mathieucarbou/ESPAsyncWebServer fork (v3.3.15, C6 support), FastLED.
- **native**: host-only, runs `test/test_ntp_packet` (see [testing.md](testing.md)).

## WiFi credentials

Copy `secrets.ini.template` to `secrets.ini` (git-ignored) and fill in `wifi_ssid` / `wifi_password`. They are compiled in as `SSID_NAME` / `SSID_PASS` build flags, so the network is fixed at **build time**: the board joins whatever was in the file when it was flashed. Reflashing with different credentials moves the board to another network and address, and a harness addressing it by IP simply stops getting answers - there is no error to see. Check the file before every upload.

## After flashing

At boot the board prints to the USB console at 115200: protocol version, the SSID it is joining, and the IP, MAC, gateway and mask it got. The SSID line is the only place the compiled-in network is visible; the MAC is there so the address can be pinned on the router (an SSID shared by two access points puts the board on whichever answers first).

If WiFi fails, `setup()` returns before any route is registered - the board is up but serves nothing, and only the console says `WiFi Failed!`. `curl http://<ip>/version` confirms the board is serving.

## Build flags and protocol version

- `METF_VERSION` comes from `metf_version` in the `[env]` section of `platformio.ini` - one place for both boards. Bump it when the HTTP protocol changes: 5 added `/read/stat`, 6 added `/ntp`. `test/board` checks the minimum version it needs (`test_protocol_version`).
- Both envs set `LOG_LEVEL_DEBUG` and `SSID_NAME` / `SSID_PASS`.
- The C6 env also sets `ESP32_C6_env`, `ARDUINO_USB_MODE=1` (native USB Serial/JTAG; the C6 has no USB-OTG), `ARDUINO_USB_CDC_ON_BOOT=1` (`Serial` → USB CDC), `ASB_BUFFER_BYTES=65536`, `ASB_MAX_LINE_LEN=128`, `RGB_DEFAULT_PIN=8`, `RGB_NUMBER=1`.
- What `ASB_*` and `RGB_*` do: [architecture.md](architecture.md).
