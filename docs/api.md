# HTTP API

Protocol version **12** (`GET /version`). The board serves plain HTTP on port 80.

- POST parameters are form fields in the body (`application/x-www-form-urlencoded`, `curl -d name=value`); GET parameters go in the query string.
- Numbers are decimal: `address=72`, not `0x48`.
- Commands answer `200 OK` (text/plain) unless noted otherwise. `/pulse` answers `202 Accepted`: the work it starts outlives the answer.

Errors:

| Code | Body | When |
|---|---|---|
| 400 | `post form parameter '<name>' not found` | a required POST field is missing |
| 400 | `parameter '<name>' not found` | a required GET parameter is missing |
| 400 | `parameter '<name>' is incorrect` | the value or `action` is not accepted |
| 404 | `Not found` | unknown URL or wrong method |
| 409 | `pulse in progress` | `/pulse` on a pin whose own pulse is still running |
| 503 | `no free pulse timer` | `/pulse` with all 8 pulse slots busy |
| 409 | `previous command in progress` | `POST /wifi action=set` while the last one is not applied yet |
| 500 | a description | hardware failure: I2C error, UDP port busy, RGB not initialised |

Contents: [Service](#service) · [Network](#network) · [GPIO](#gpio) · [I2C](#i2c) · [Serial log](#serial-log) · [NTP server](#ntp-server) · [RGB LED](#rgb-led)

## Service

### GET /ping

Answers `pong`.

### GET /version

Answers the protocol version, e.g. `11`. It changes when the API changes: 5 added `/read/stat`, 6 added `/ntp`, 7 made `/pulse` non-blocking and gave it `409`, 8 made `/pulse` answer at once with `202` instead of at the end of the pulse, 9 added `/wifi` and the setup page at `/`, and gave `/rgb` the `status` action, 10 added `problem` to `GET /wifi` - the disconnect reason in words, 11 put the `X-Uptime-Ms` header on every answer, 12 gave `/read` the `ack` parameter and the `X-Log-Seq` header, so the board keeps lines until the reader confirms them.

### X-Uptime-Ms

Every answer carries `X-Uptime-Ms`: milliseconds since the board booted, taken
from `millis()`.

A bench keeps the last value it saw. A smaller value than the one before means
the board restarted between the two requests - and that a restart is the likely
reason for whatever else looks wrong: the log ring was emptied, the NTP server
went off, pins went back to inputs. Nothing else tells a restart from a board
that was merely busy: `offline_s` in `GET /wifi` counts from the loss of the
network **or** from boot, and the counters next to it are reset by a restart too.

The value wraps after 49 days of uptime, which a bench reads as one false
restart.

## Network

The board keeps its own network settings and can be moved to another router without reflashing: see [wifi.md](wifi.md) for the algorithm, the timings and the setup page.

### GET /wifi

Answers JSON. The password is never returned.

```json
{"state":"online","mode":"sta","connected":true,"ssid":"lab","source":"build",
 "ip":"192.168.1.50","rssi":-68,"channel":4,"fast":true,"ap_ssid":"METF-AB12",
 "ap_up":false,"ap_clients":0,"offline_s":0,"attempts":0,"last_reason":0,"problem":"none",
 "scanning":false,"hw_error":false,"pending":false}
```

| Field | Meaning |
|---|---|
| `state` | `starting`, `connecting`, `online`, `lost`, `ap` |
| `mode` | radio mode: `sta`, `ap`, `ap_sta` |
| `connected` | the board has an IP on the router |
| `ssid` | the network the board connects to |
| `source` | `build` - from `secrets.ini`; `saved` - set through the portal or `action=set` |
| `ip` | address on the router, empty while not connected |
| `rssi`, `channel` | signal and current radio channel |
| `fast` | a channel/BSSID pair is saved, so the next connect skips the scan |
| `ap_ssid`, `ap_up`, `ap_clients` | the board's own access point: its name, whether it is on the air, and how many stations are on it |
| `offline_s` | seconds since the network was lost (or since boot) |
| `attempts` | failed connect attempts since the last success |
| `last_reason` | disconnect reason code of the core |
| `problem` | the same reason in words, for a human: `none`, `password` (the network did not accept the password), `not_found` (no such network on the air - switched off, renamed, out of range, or on a security mode the board cannot join), `dropped` (the link died on the router's side: powered off, rebooting, or too far), `other` (a code that says nothing certain). Always `none` while connected. The setup page shows the matching phrase instead of the code; how codes map to causes is [wifi.md, P9](wifi.md) |
| `hw_error` | the access point did not start, or a flash write failed |
| `scanning` | a scan is running: the radio is walking the channels, and everything the board answers - including NTP - waits for it |
| `pending` | a command was accepted and `loop()` has not applied it yet |

### POST /wifi

| `action` | Fields | What the board does |
|---|---|---|
| `set` | `ssid` (1-32), `password` (empty or 8-63) | save the network and connect to it now. The saved network overrides the compiled one |
| `forget` | - | erase the saved network and go back to the one from `secrets.ini` |
| `ap` | `value` - `0` to drop it | raise the board's own access point now. `value=0` says it is no longer needed: it goes down once the board is on the network and nobody is connected to it |
| `scan` | - | refresh the list of networks shown on the setup page. It takes the radio for a couple of seconds - watch `scanning` in `GET /wifi` and do not time anything else across it |

Answers `202 accepted`: the command is applied from the main loop, not in the handler. Watch `GET /wifi` for the result - `pending` goes false once the loop has taken the command, and `state` changes when it has done its work.

`set` with a bad `ssid` or `password` answers `400`, and a second `set` before the first is applied answers `409`.

### GET /

The setup page: current state, the networks the board can see, and a form for a new one. Plain HTML, no JavaScript, so that a phone's captive browser can use it. The page is served on both interfaces, so the network can also be changed from the bench.

While the board is connecting or scanning the page refreshes itself every 2 seconds. Once connected it shows the new IP address in large type - the bench addresses the board by it.

A client of the board's own access point that asks for any other URL gets a redirect to this page, which is what makes the phone open it by itself.

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

The board sets the pin to `OUTPUT`, writes `value`, arms a timer for `duration_ms` and answers **immediately**: `202 Accepted`, body `duration_ms`. When the timer fires, the pin goes back to `INPUT` (high-Z). Nothing is sent at that moment - **the client waits out the pulse on its own clock**, and the board stays responsive throughout: `/read`, `/ping` and everything else keep answering.

The answer is a receipt, not a finish line. Protocol 7 made it the finish line - the answer was held back until the timer fired - and that turned out to be a bad clock: a deferred answer is not sent when it is ready but on the connection's next poll, and AsyncTCP polls about twice a second. Measured on the board, 20 ms pulse, 20 samples: the answer arrived 240-336 ms after the line was already released. A bench that spaces impulses from the moment the call returns silently got a quarter-second added to every gap. Protocol 8 hands the clock back to the client, where it is exact.

Two pulses cannot overlap **on the same pin**: a second `/pulse` for a pin whose pulse is still running is refused with `409 pulse in progress`. That refusal is also how a client can ask whether a line is still busy without touching it.

Different pins run at the same time - the board keeps 8 pulse slots, one timer each - because that is what a bench does: press the button while a train of impulses runs into the counter input. Protocol 7 had a single flag for the whole board and refused such a press; with all 8 slots busy the answer is `503 no free pulse timer`.

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

Returns every complete line received since the previous read. Each line ends with `\n`; `\r` is dropped. A line still being received (no newline yet) stays for the next read.

A line longer than `line_len - 1` characters is split into several lines; the reader has to glue them back.

Every answer carries the number of its last line in the `X-Log-Seq` header. Lines are numbered from 1 and the counter keeps growing for the life of the board, evicted lines included.

| Request | What the board does with the lines it just handed out |
|---|---|
| `GET /read?ack=<n>` | forgets everything up to `n`, **keeps** what it returns |
| `GET /read` | forgets them at once |

**Use `ack`.** Without it a lost answer costs the lines for good: the board marks them read while the response is still being assembled, and the `200` cannot confirm anything - it arrives after the sending and may be lost itself. With `ack` the reader repeats the same request and gets the same window again.

The reader sends back the `X-Log-Seq` it received; `ack=0` confirms nothing. A number larger than anything in the ring simply empties it - that is what a reader that outlived a reboot of the board sends.

Lines held for confirmation take up ring space. The reader is expected to confirm on its next request; a reader that stops confirming will see `dropped` grow.

The plain form stays for protocol 11 readers and for `curl` by hand.

### GET /read/stat

State of the buffer as JSON:

```json
{"lines":12,"seq":345,"dropped":0,"baud":115200,"capacity":511,"line_len":128,"bytes":65536}
```

| Field | Meaning |
|---|---|
| `lines` | lines waiting to be read, including those held for confirmation |
| `seq` | number of the last line the board accepted |
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

The onboard LED normally shows what the firmware is doing (colours and rhythms: [README](../README.md#status-led)). `POST /rgb` takes it away from that and gives it to the bench.

Built for the ESP32-C6 with `RGB_DEFAULT_PIN` (its onboard WS2812B on GPIO 8) and for a plain LED with `STATUS_LED_PIN` (the NodeMCU build drives GPIO 2, which has no colour: any non-black colour means "lit").

### POST /rgb

| `action` | Fields | What the board does |
|---|---|---|
| `begin` | `pin`, `number` - accepted but ignored | take the LED from the status display and turn it off. Pin and LED count are fixed at build time |
| `brightness` | `value` - 0-255 | set the brightness of the bench's colour |
| `color` | `value` - `RRGGBB` hex, e.g. `FF0000` | light this colour |
| `status` | - | give the LED back to the status display |

`brightness` and `color` before `begin` fail with `500 RGB not initialized. Call action=begin first`. A reboot also returns the LED to the status display.
