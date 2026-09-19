# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

ESPTestFramework (METF): firmware for ESP8266/ESP32-C6 that turns the board into an HTTP-controlled test bench for a device under test (mainly the Waterius). PlatformIO project, default env `esp32-c6-super-mini`.

## Where to read

- [docs/build.md](docs/build.md) - building, flashing, WiFi credentials, boot console, build flags, protocol version. Read before any build or upload.
- [docs/testing.md](docs/testing.md) - the three test layers (host, on-device, live board via pytest) and how to run one test.
- [docs/architecture.md](docs/architecture.md) - how the firmware works: serial ports, HTTP routes and their traps, serial log buffer, NTP server, RGB, logging. Read before changing `src/`.
- [docs/api.md](docs/api.md) - every HTTP URL with its parameters, responses and errors. Read before calling the board or changing a route.
- [docs/wifi.md](docs/wifi.md) - the network: principles, the boot and reconnect algorithm, the Espressif behaviour it relies on, measurements and known gaps. Read before touching anything about WiFi, and when auditing it.
- [README.md](README.md) - short overview, examples, list of URLs.

## Conventions

- Newer code comments, docstrings and pytest messages are written in Russian; match the surrounding file.
- A route added, removed or changed means updating `docs/api.md` and the URL table in `README.md`, and bumping `metf_version` in `platformio.ini` if clients see the difference.
