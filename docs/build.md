# Build, flash, configure

`pio` may not be on the shell PATH; the PlatformIO install lives at `~/.platformio/penv/bin/pio`.

## Build and upload

```bash
pio run                                   # default env: esp32-c6-super-mini
pio run -e nodemcuv2
pio run -e esp32-c6-super-mini -t upload --upload-port /dev/cu.usbmodemXXXX
pio device monitor -e esp32-c6-super-mini --port /dev/cu.usbmodemXXXX
```

## Which port is which board

A `/dev/cu.*` name belongs to the USB socket, not to the board: replug the cable
and the same name lands on the neighbour. That is why no port is written into
`platformio.ini`.

Leaving the choice to PlatformIO is worse than it looks. With no `upload_port`
it takes the first port whose VID:PID is known to any installed platform
([`finder.py`](https://github.com/platformio/platformio-core/blob/v6.2.0/platformio/device/finder.py#L217),
`_find_known_device`) - on a bench with several boards attached that is whichever
one the OS happened to enumerate first, and the upload goes there silently.

What does identify a board is its USB serial number: on an ESP32 it is the MAC -
the same one the board prints at boot - and on a USB-UART bridge a string from
its EEPROM. `pio device list` shows it as `SER=`:

```
/dev/cu.usbmodemXXXX  SER=AA:BB:CC:DD:EE:FF  USB JTAG/serial debug unit
```

Name the board once in `secrets.ini` (git-ignored, one file per machine) and
`pio run -t upload` finds its port by itself, whatever the socket:

```ini
[board_serial]
esp32-c6-super-mini = AA:BB:CC:DD:EE:FF
nodemcuv2 = 0001
```

`--upload-port` still wins over everything, and `METF_BOARD_SERIAL` overrides the
file for one command. Without a binding, a single attached board is uploaded as
before, while several stop the build with the port list instead of guessing:
[`scripts/pick_port.py`](../scripts/pick_port.py).

`pio device monitor` does not run project scripts
([`command.py`](https://github.com/platformio/platformio-core/blob/v6.2.0/platformio/device/monitor/command.py#L127)),
so it still needs `--port`. Picking the wrong one there only opens a console on
another board - and resets it, since opening the port toggles DTR/RTS.

## When a build suddenly cannot find a library header

`ESPAsyncTCP.h: No such file or directory` (or the same for any other
dependency) is almost never a missing declaration. `lib_deps` names the library
directly used; its own dependencies come from the library's `library.json` -
`ESP Async WebServer` declares `ESPAsyncTCP` for `espressif8266` and `AsyncTCP`
for `espressif32`, and PlatformIO installs them by itself.

What it really means is that `.pio/libdeps/<env>/` is half-installed: an install
interrupted once (a dropped download, a failed `package-postinstall.py`) leaves
the directly named library in place and the transitive one missing, and every
later build reuses that state instead of repairing it. Wipe the directory and
build again:

```bash
rm -rf .pio/libdeps/nodemcuv2
pio run -e nodemcuv2
```

Adding the missing dependency to `lib_deps` also makes the error go away, and
that is the trap: it hides a broken local directory behind a redundant line in
the config, where it survives into the repository and outlives the cause.

## Environments

- **ESP8266** (`nodemcuv2`): `espressif8266@4.2.1`, `ESPAsyncTCP`, ESP Async WebServer 1.2.3.
- **ESP32-C6** (`esp32-c6-super-mini`, default): pioarduino `platform-espressif32`, `AsyncTCP`, mathieucarbou/ESPAsyncWebServer fork (v3.3.15, C6 support), FastLED.
- **native**: host-only, runs `test/test_ntp_packet` (see [testing.md](testing.md)).

## WiFi credentials

Copy `secrets.ini.template` to `secrets.ini` (git-ignored) and fill in `wifi_ssid` / `wifi_password`. They are compiled in as `SSID_NAME` / `SSID_PASS` build flags and are the board's **default** network.

A network set through the setup page or `POST /wifi action=set` is saved in the board's own flash (NVS on ESP32, EEPROM on ESP8266) and **overrides the compiled one**, including after a reflash - flashing does not erase NVS. So a board that has been set up by hand keeps that network until:

```bash
curl -X POST http://<board ip>/wifi -d action=forget   # back to secrets.ini
```

The boot line says which one is in force: `wifi: ssid <name> (build)` or `(saved)`. `GET /wifi` answers the same in its `source` field. If a board ignores the credentials you just flashed, that line is where you find out why.

Credentials never leave the board: the password is not in `GET /wifi`, not on the page and not in the console.

## After flashing

At boot the board prints to the USB console at 115200: the protocol version, then one line with the network it will join, where that network came from, whether a channel is saved for a fast connect, its MAC and the name of its own access point. The MAC is there so the address can be pinned on the router.

The first connect attempt is at five seconds, the second at fifteen, and if both fail the board raises its own access point at about twenty-five seconds and says so with the address of the setup page. The server is up the whole time, it just has no address on the router yet.

While the network is down the console carries it: `wifi: disconnected, reason 201 NO_AP_FOUND` at the moment it happens, one summary line a minute after that, and the address when it comes back. The LED says the same without a console (see [architecture.md](architecture.md#status-led)), and `curl http://<ip>/version` confirms the board is serving.

Moving the board to another network without a cable: connect a phone to `METF-XXXX` (open), the setup page opens by itself, pick the network, type its password, and the page shows the new address of the board when it joins. Holding BOOT for three seconds raises that access point on demand.

## Build flags and protocol version

- `METF_VERSION` comes from `metf_version` in the `[env]` section of `platformio.ini` - one place for both boards. Bump it when the HTTP protocol changes: 5 added `/read/stat`, 6 added `/ntp`, 7 made `/pulse` non-blocking and gave it `409`, 8 made `/pulse` answer at once with `202`, 9 added `/wifi` and the setup page and gave `/rgb` the `status` action. `test/board` checks the minimum version it needs (`test_protocol_version`).
- Both envs set `LOG_LEVEL_DEBUG` and `SSID_NAME` / `SSID_PASS`.
- The C6 env also sets `ESP32_C6_env`, `ARDUINO_USB_MODE=1` (native USB Serial/JTAG; the C6 has no USB-OTG), `ARDUINO_USB_CDC_ON_BOOT=1` (`Serial` → USB CDC), `ASB_BUFFER_BYTES=65536`, `ASB_MAX_LINE_LEN=128`, `RGB_DEFAULT_PIN=8` (the onboard WS2812B) and `BUTTON_PIN=9` (BOOT).
- The NodeMCU env sets `STATUS_LED_PIN=2` (the module's blue LED, lit by a low level) and `BUTTON_PIN=0` (FLASH).
- What `ASB_*`, `RGB_DEFAULT_PIN`, `STATUS_LED_PIN` and `BUTTON_PIN` do: [architecture.md](architecture.md).
