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

### Configuration

Before building, copy `secrets.ini.template` to `secrets.ini` and fill in WiFi credentials:

```ini
[secrets]
wifi_ssid=YourWiFiSSID
wifi_password=YourWiFiPassword
```

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
- `/i2c` - I2C communication with actions: begin, setClock, setClockStretchLimit, ask, flush
- `/serial` - Serial port configuration (baudrate switching)
- `/read` - Read accumulated serial data from AsyncSerialBuffer
- `/version` - Get framework version

**Important I2C handling differences:**
- ESP8266 uses `Wire.setClockStretchLimit()` with microseconds
- ESP32/ESP32-C6 uses `Wire.setTimeOut()` with milliseconds (the code automatically converts)

#### 2. **AsyncSerialBuffer (`AsyncSerialBuffer.h/cpp`)**

A thread-safe circular buffer for capturing serial data asynchronously:

- Accumulates incoming serial data in `loop()` without blocking web requests
- Stores lines in a ring buffer (`ASB_MAX_LINES` × `ASB_MAX_LINE_LEN`)
- Uses critical sections for thread safety:
  - ESP32: FreeRTOS spinlocks (`portENTER_CRITICAL`/`portEXIT_CRITICAL`)
  - ESP8266: Interrupt disable (`noInterrupts()`/`interrupts()`)
- Handles line overflow by auto-wrapping and evicting oldest lines
- `drain_to()` outputs all accumulated lines and clears the buffer

**Configuration via build flags:**
- `-DASB_MAX_LINES=100` - Maximum number of buffered lines
- `-DASB_MAX_LINE_LEN=60` - Maximum characters per line

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

#### 5. **RGB LED Control (ESP32 only)**

WS2812B addressable LED strip support via FastLED library:

- `/rgb` endpoint with actions: begin, brightness, color
- Supports 1-10 LEDs
- Default pin: GPIO 8 (configurable via `RGB_DEFAULT_PIN` define in `main.cpp`)
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
- Can reinitialize with different pin/count
- Previous LED state cleared on reinitialization

### HTTP Request/Response Patterns

- **GET requests:** Use query parameters (e.g., `/digitalRead?pin=5`)
- **POST requests:** Use form-encoded parameters (e.g., `/digitalWrite` with `pin=5&value=1`)
- **Error responses:**
  - 400: Missing or incorrect parameters
  - 404: Unknown endpoint
  - 500: Hardware/operation failures (e.g., I2C transmission errors)

### Build Flags

Standard build flags defined in `platformio.ini`:

- `-DMETF_VERSION="2"` - Protocol version (exposed via `/version` endpoint)
- `-DLOG_LEVEL_DEBUG` - Enable debug logging
- `-DSSID_NAME` / `-DSSID_PASS` - WiFi credentials from `secrets.ini`
- `-D ESP32_C6_env` - ESP32-C6 specific flag

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
- Upload port: `/dev/cu.usbmodem21301`
- Monitor port: `/dev/cu.usbserial-2110`
- Upload speed: 460800
- Library: mathieucarbou/ESPAsyncWebServer (fork with C6 support)
- Uses custom platform: https://github.com/pioarduino/platform-espressif32

## Testing Workflow

1. Upload firmware to ESP board with WiFi credentials configured
2. Power on board - it connects to WiFi and starts HTTP server
3. Use HTTP client (Python scripts, curl, etc.) to send commands
4. Check board's IP address in serial monitor output on startup
