# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESPTestFramework is an HTTP-based hardware testing framework that runs on ESP8266/ESP32 boards. It exposes GPIO, I2C, and Serial functionality through a web server, allowing test scripts (typically Python) to control and test external hardware connected to the ESP board.

## Build and Development Commands

### Building and Uploading

```bash
# Build for the default environment (esp32-c6-super-mini)
pio run

# Build for a specific environment
pio run -e nodemcuv2
pio run -e esp32-c6-super-mini

# Upload to device
pio run -t upload

# Upload to specific environment
pio run -e nodemcuv2 -t upload
pio run -e esp32-c6-super-mini -t upload

# Build and upload in one command
pio run -t upload -e nodemcuv2
```

### Monitoring Serial Output

```bash
# Monitor serial output (default environment)
pio device monitor

# Monitor with specific environment settings
pio device monitor -e nodemcuv2
pio device monitor -e esp32-c6-super-mini
```

### Running Tests

```bash
# Run unit tests (uploads test firmware to device)
pio test -e nodemcuv2

# Run tests for specific environment
pio test -e esp32-c6-super-mini
```

### Configuration

Before building, copy `secrets.ini.template` to `secrets.ini` and fill in WiFi credentials:

```ini
[secrets]
wifi_ssid=YourWiFiSSID
wifi_password=YourWiFiPassword
```

`secrets.ini` is git-ignored - credentials never go into the repository. They are
compiled into the firmware as `SSID_NAME` / `SSID_PASS` build flags, so the
network is fixed at **build time**: the board joins whatever was in the file when
it was flashed. Reflashing with different credentials moves the board to another
network and another address, and a harness addressing it by IP simply stops
getting answers - there is no error to see. Check the file before every upload.

The upload port belongs on the command line, not in `platformio.ini` - the value
there goes stale as soon as the board is replugged:

```bash
ls /dev/cu.*     # ESP32-C6 SuperMini is a native USB CDC port: usbmodem*
pio run -e esp32-c6-super-mini -t upload --upload-port /dev/cu.usbmodemXXXX
```

At boot the board prints its protocol version, the SSID it is joining and the
address it got, to the USB console at 115200. The SSID line is the one to read
after an upload - it is the only place the compiled-in network is visible.
`curl http://<ip>/version` confirms the board is up.

## Architecture and Code Structure

### Multi-Platform Support

The codebase supports two ESP platform families with conditional compilation:

- **ESP8266** (NodeMCU, D1 Mini Pro): Uses `espressif8266` platform with `ESPAsyncTCP` library
- **ESP32** (ESP32-C6 SuperMini): Uses custom ESP32 platform with `AsyncTCP` library

Platform-specific code uses preprocessor directives (`#ifdef ESP32`, `#ifdef ESP8266`).

### Key Components

#### 1. **Web Server (`main.cpp`)**

The main application implements an `AsyncWebServer` on port 80 with HTTP endpoints for hardware control:

- `/ping` - Connectivity test
- `/pinMode`, `/digitalRead`, `/digitalWrite` - GPIO control
- `/pulse` - Drive a pin to a value for `duration_ms`, then release it to high-Z (INPUT); precise ESP-side timing for simulating button presses
- `/i2c` - I2C communication with actions: begin, setClock, setClockStretchLimit, ask, flush
- `/serial` - Serial port configuration (baudrate switching)
- `/read` - Read accumulated serial data from AsyncSerialBuffer
- `/read/stat` - Ring buffer state as JSON: `lines`, `dropped`, `baud`, `capacity`, `line_len`, `bytes`. Registered **before** `/read`: `AsyncCallbackWebHandler::canHandle` also matches by prefix (`url.startsWith(_uri + "/")`), so `/read` declared first would swallow `/read/stat`
- `/version` - Get framework version

**Important I2C handling differences:**
- ESP8266 uses `Wire.setClockStretchLimit()` with microseconds
- ESP32/ESP32-C6 uses `Wire.setTimeOut()` with milliseconds (the code automatically converts)

#### 2. **AsyncSerialBuffer (`AsyncSerialBuffer.h/cpp`)**

A thread-safe circular buffer for capturing serial data asynchronously:

- Accumulates incoming serial data in `loop()` without blocking web requests
- Stores lines in a ring buffer sized by one constant, `ASB_BUFFER_BYTES`; `ASB_MAX_LINES` is derived from it and `ASB_MAX_LINE_LEN`
- Uses critical sections for thread safety:
  - ESP32: FreeRTOS spinlocks (`portENTER_CRITICAL`/`portEXIT_CRITICAL`)
  - ESP8266: Interrupt disable (`noInterrupts()`/`interrupts()`)
- Handles line overflow by auto-wrapping and evicting oldest lines
- Counts evicted lines in `dropped()`; the counter resets on `flush()` and is exposed via `/read/stat`. Eviction is silent otherwise, and a silently shortened log makes tests green for the wrong reason
- `drain_to()` outputs all accumulated lines and clears the buffer

**Configuration via build flags:**
- `-DASB_BUFFER_BYTES=6000` - Bytes given to the log; the number of lines is derived from it
- `-DASB_MAX_LINE_LEN=60` - Maximum characters per line
- `-DASB_MAX_LINES=100` - Number of lines, if set directly it wins over the derived value

Defaults suit the ESP8266. `esp32-c6-super-mini` sets 64 KB / 128 characters (511 lines) - a full Waterius session fits without eviction.

#### 3. **Logging System (`logging.h`)**

Macro-based logging with compile-time level control:

- **Build flags:** `-DLOG_LEVEL_ERROR`, `-DLOG_LEVEL_INFO`, `-DLOG_LEVEL_DEBUG`
- **Macros:** `LOG_ERROR()`, `LOG_INFO()`, `LOG_DEBUG()`
- Uses stream operators (`<<`) for flexible output formatting
- Includes millisecond-precision timestamps (HH:MM:SS:mmm)

**Important:** Logging uses the same Serial port as AsyncSerialBuffer. When debugging is enabled, log output will be captured by the buffer.

#### 4. **Utility Functions (`lib/utils/src/utils.h`)**

Helper functions for I2C data formatting:

- `hexCharToInt()` - Convert hex character to integer
- `intToHexChar()` - Convert integer to hex character
- `hexText2AsciiArray()` - Convert hex string to byte array for I2C transmission

#### 5. **RGB LED Control (ESP32 only, optional)**

WS2812B addressable LED strip support via FastLED library.

**Enabling RGB support:**
- RGB support is optional and only compiled when `RGB_DEFAULT_PIN` is defined in build flags
- To enable: Add `-DRGB_DEFAULT_PIN=<pin>` to your build flags (e.g., `-DRGB_DEFAULT_PIN=8`)
- To disable: Simply omit the `RGB_DEFAULT_PIN` build flag - this saves memory and code space

**Configuration:**
- `/rgb` endpoint with actions: begin, brightness, color
- Number of LEDs configured at compile-time via `RGB_NUMBER` build flag (default: 1)
- GPIO pin configured at compile-time via `RGB_DEFAULT_PIN` build flag (required to enable RGB)
- Thread-safe LED updates using critical sections (same pattern as AsyncSerialBuffer)
- Static memory allocation for predictable performance

**Important ESP32-C6 considerations:**
- FastLED 3.7.0+ required for ESP32-C6 timing fixes
- Uses RMT driver automatically for WS2812B
- Critical sections prevent async web server from corrupting LED timing
- Pin is compile-time constant (FastLED limitation) - change `RGB_DEFAULT_PIN` and recompile to use different pin

**Color format:**
- 6-character hex strings only (e.g., "FF0000" for red)
- RGB byte order in API: RR GG BB
- WS2812B uses GRB wire protocol (handled automatically by FastLED)

**Initialization:**
- Must call `action=begin` before brightness or color actions
- Pin and LED count are compile-time constants - change build flags and recompile to modify
- LED array is statically pre-allocated at compile-time
- Previous LED state cleared on reinitialization (begin action)

**Static Initialization:**
- FastLED is initialized using static memory allocation via `FastLED.addLeds<WS2812B, RGB_DEFAULT_PIN, GRB>(rgb_leds, RGB_NUMBER)`
- Pin number and LED count are template/macro parameters that must be known at compile-time
- Configure via build flags in platformio.ini:
  - `-DRGB_DEFAULT_PIN=<pin>` - GPIO pin for WS2812B data line
  - `-DRGB_NUMBER=<count>` - Number of LEDs in the strip

### HTTP Request/Response Patterns

- **GET requests:** Use query parameters (e.g., `/digitalRead?pin=5`)
- **POST requests:** Use form-encoded parameters (e.g., `/digitalWrite` with `pin=5&value=1`)
- **Error responses:**
  - 400: Missing or incorrect parameters
  - 404: Unknown endpoint
  - 500: Hardware/operation failures (e.g., I2C transmission errors)

### Build Flags

Standard build flags defined in `platformio.ini`:

- `-DMETF_VERSION="5"` - Protocol version (exposed via `/version` endpoint); 5 added `/read/stat`
- `-DLOG_LEVEL_DEBUG` - Enable debug logging
- `-DSSID_NAME` / `-DSSID_PASS` - WiFi credentials from `secrets.ini`
- `-D ESP32_C6_env` - ESP32-C6 specific flag
- `-DARDUINO_USB_MODE=1` - Use the native USB Serial/JTAG CDC (ESP32-C6 has no USB-OTG)
- `-DARDUINO_USB_CDC_ON_BOOT=1` - Route `Serial` to the native USB CDC, enabled on boot
- `-DASB_BUFFER_BYTES=<bytes>` - Size of the serial log ring buffer (default 6000)
- `-DASB_MAX_LINE_LEN=<chars>` - Characters per buffered line (default 60)
- `-DRGB_DEFAULT_PIN=<pin>` - GPIO pin for RGB LED data line (ESP32 only, optional - required to enable RGB support)
- `-DRGB_NUMBER=<count>` - Number of WS2812B LEDs in the strip (ESP32 only, optional, default: 1)

### Serial Port Handling

The framework dynamically switches serial baudrates via `/serial` endpoint:

- Allowed baudrates: 300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 74880, 115200, 230400, 250000, 460800, 921600
- Baudrate changes use critical sections to prevent race conditions with AsyncSerialBuffer
- Default: 115200 baud

### Environment-Specific Settings

#### ESP8266 (nodemcuv2)
- Upload port: `/dev/cu.usbserial-0001`
- Monitor port: `/dev/cu.usbserial-0001`
- Upload speed: 230400
- Library: ESP Async WebServer 1.2.3

#### ESP32-C6 (esp32-c6-super-mini)
- Upload port: `/dev/cu.usbmodem1201`
- Monitor port: `/dev/cu.usbserial-2110`
- Upload speed: 460800
- Library: mathieucarbou/ESPAsyncWebServer (fork with C6 support)
- Uses custom platform: https://github.com/pioarduino/platform-espressif32

## Testing Workflow

1. Upload firmware to ESP board with WiFi credentials configured
2. Power on board - it connects to WiFi and starts HTTP server
3. Use HTTP client (Python scripts, curl, etc.) to send commands
4. Check board's IP address in serial monitor output on startup
