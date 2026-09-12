# HTTP API

Protocol version **6** (`GET /version`). The board serves plain HTTP on port 80.

- POST parameters are form fields in the body (`application/x-www-form-urlencoded`, `curl -d name=value`); GET parameters go in the query string.
- Numbers are decimal: `address=72`, not `0x48`.
- Commands answer `200 OK` (text/plain) unless noted otherwise.

Errors:

| Code | Body | When |
|---|---|---|
| 400 | `post form parameter '<name>' not found` | a required POST field is missing |
| 400 | `parameter '<name>' not found` | a required GET parameter is missing |
| 400 | `parameter '<name>' is incorrect` | the value or `action` is not accepted |
| 404 | `Not found` | unknown URL or wrong method |
| 500 | a description | hardware failure: I2C error, UDP port busy, RGB not initialised |

Contents: [Service](#service) · [GPIO](#gpio) · [I2C](#i2c) · [Serial log](#serial-log) · [NTP server](#ntp-server) · [RGB LED](#rgb-led)

## Service

### GET /ping

Answers `pong`.

### GET /version

Answers the protocol version, e.g. `6`. It changes when the API changes: 5 added `/read/stat`, 6 added `/ntp`.

## GPIO

### POST /pinMode

| Field | Meaning |
|---|---|
| `pin` | GPIO number |
| `mode` | mode constant of the board's Arduino core, passed to `pinMode()` as is |

The constants differ between the cores:

| Mode | ESP8266 | ESP32 |
|---|---|---|
| `INPUT` | 0 | 1 |
| `OUTPUT` | 1 | 3 |
| `INPUT_PULLUP` | 2 | 5 |
| `INPUT_PULLDOWN` | 4 (`INPUT_PULLDOWN_16`, GPIO16 only) | 9 |
| `OUTPUT_OPEN_DRAIN` | 3 | 19 |

### GET /digitalRead

`/digitalRead?pin=<n>` answers `1` or `0`.

### POST /digitalWrite

| Field | Meaning |
|---|---|
| `pin` | GPIO number |
| `value` | `1` high, `0` low |

Does not change the pin mode; set it with `/pinMode` first.

### POST /pulse

Drives a pin for a fixed time and releases it - a button press with the timing done on the ESP.

| Field | Meaning |
|---|---|
| `pin` | GPIO number |
| `value` | level to drive, `1` or `0` |
| `duration_ms` | how long to hold it, ms |

The board sets the pin to `OUTPUT`, writes `value`, waits `duration_ms`, then sets the pin to `INPUT` (high-Z). The answer comes after the pulse ends, and the web server handles no other request in the meantime.

## I2C

### POST /i2c

Every call takes `action`; the other fields depend on it.

| `action` | Fields | What the board does |
|---|---|---|
| `begin` | `sda_pin`, `scl_pin` - optional, both or neither | `Wire.begin(sda, scl)`; the board's default SDA/SCL without them |
| `setClock` | `value` - Hz | `Wire.setClock(value)` |
| `setClockStretchLimit` | `value` - µs | ESP8266: `Wire.setClockStretchLimit(value)`. ESP32: `Wire.setTimeOut(value / 1000)` ms, at least 1000 ms |
| `ask` | `address`, `hexstring`, `response` | write, then read; see below |
| `flush` | - | `Wire.flush()` |

`ask` fields:

| Field | Meaning |
|---|---|
| `address` | 7-bit slave address, decimal |
| `hexstring` | bytes to send as hex, even length: `4D` is one byte `0x4D`. Up to 255 bytes |
| `response` | how many bytes to read back, 0-255 |

The board sends `hexstring` in one transmission, waits 1 ms, then reads the answer one byte per `requestFrom(address, 1)`. It answers with the received bytes as upper-case hex, e.g. `01FF`.

| Error | Meaning |
|---|---|
| 400 `parameter 'hexstring' is incorrect` | not hex, or odd length |
| 500 `i2c write error` | `Wire.write()` refused a byte |
| 500 `i2c end transmission error <n>` | `Wire.endTransmission()` failed: 1 data too long, 2 NACK on address, 3 NACK on data, 4 other |
| 500 `i2c read timeout. Received: <hex>` | the slave stopped answering; `<hex>` is what arrived before that |

## Serial log

The board records everything the DUT sends on its UART into a ring buffer. It listens on `Serial0` (UART0 pins) on ESP32-C6, where `Serial` is the USB console, and on `Serial` (UART0) on ESP8266. Default speed 115200.

### POST /serial

| Field | Meaning |
|---|---|
| `baudrate` | UART speed. **Optional, but omitting it means 115200** - a call with only `flush=1` also resets the speed |
| `flush` | `1` - empty the buffer and reset the `dropped` counter |

Allowed speeds: 300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 74880, 115200, 230400, 250000, 460800, 921600. Anything else is `400 Invalid speed`.

The fields may be sent in the body or in the query string. The answer is `Set 9600 baudrate` or `Baudrate is 9600` (already set), followed by `, flush buffer` when the buffer was cleared.

### GET /read

Returns every complete line received since the previous read and empties the buffer. Each line ends with `\n`; `\r` is dropped. A line still being received (no newline yet) stays for the next read.

A line longer than `line_len - 1` characters is split into several lines; the reader has to glue them back.

### GET /read/stat

State of the buffer as JSON:

```json
{"lines":12,"dropped":0,"baud":115200,"capacity":511,"line_len":128,"bytes":65536}
```

| Field | Meaning |
|---|---|
| `lines` | lines waiting to be read |
| `dropped` | lines evicted since the last flush because the ring was full |
| `baud` | current UART speed |
| `capacity` | how many lines the ring holds |
| `line_len` | characters per line, including the terminator |
| `bytes` | total size of the ring |

**`dropped > 0` means the log has a hole in it.** Eviction is silent: a test reading the log after that sees a shorter log, not an error, and goes green for the wrong reason. Check the counter before trusting the log.

Capacity is set at build time: 99 lines of 60 on ESP8266, 511 lines of 128 on ESP32-C6. See `ASB_*` in [build.md](build.md).

## NTP server

ESP32 only. The board answers NTP on UDP 123 with the time the test assigned; it has no clock of its own and needs no internet. The server is off after boot.

### POST /ntp

| `action` | Fields | What the board does |
|---|---|---|
| `start` | `epoch` | listen on UDP 123 and answer with this time; if already listening, only sets the time. 500 `unable to listen on udp 123` if the port can't be taken |
| `time` | `epoch` | move the clock, keep listening |
| `stop` | - | close the port; a client gets "port unreachable" |
| `drop` | `value` - `1` or `0` | keep the port but stay silent; a client waits for its timeout, like an unreachable internet server |

`epoch` is unix time in seconds, non-zero. After that moment the clock runs from the board's `millis()`. Answers `ok`.

Only NTP client requests (mode 3, at least 48 bytes) get an answer; anything else is counted as `ignored`. The reply comes from port 123 with leap indicator 0, stratum 1, reference id `METF`, the client's NTP version, and the client's transmit stamp in the originate field.

### GET /ntp/stat

```json
{"running":true,"dropping":false,"epoch":1767225612,"requests":3,"replies":3,"dropped":0,"ignored":0,"last_epoch":1767225610,"last_client":"192.168.1.42"}
```

| Field | Meaning |
|---|---|
| `running` | listening on UDP 123 |
| `dropping` | silent mode is on |
| `epoch` | the board's time now; counts from boot until a time has been set |
| `requests` | packets received |
| `replies` | answers sent |
| `dropped` | requests left unanswered because of silent mode |
| `ignored` | packets that were not an NTP client request |
| `last_epoch` | time put into the last answer |
| `last_client` | address of the last sender |

Counters are not reset by `stop`/`start`; compare values before and after.

## RGB LED

ESP32 only, and only in firmware built with `RGB_DEFAULT_PIN` (the ESP32-C6 SuperMini build drives its onboard LED on GPIO 8).

### POST /rgb

| `action` | Fields | What the board does |
|---|---|---|
| `begin` | `pin`, `number` - accepted but ignored | initialise the strip and turn it off. Pin and LED count are fixed at build time (`RGB_DEFAULT_PIN`, `RGB_NUMBER`) |
| `brightness` | `value` - 0-255 | set the brightness |
| `color` | `value` - `RRGGBB` hex, e.g. `FF0000` | set every LED to this colour |

`brightness` and `color` before `begin` fail with `500 RGB not initialized. Call action=begin first`.
