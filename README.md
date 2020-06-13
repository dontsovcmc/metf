# ESPTestFramework
Test your hardware by ESP using HTTP requests.

I need to test AVR board firmware. I connect ESP8266 to AVR, run web server and coding Python test scripts.

### Equipment
- ESP board. It will be a HTTP web server controlled by PC
- Python or another HTTP request stuff to write a tests

Default board: NodeMCU.
Change platformio.ini file to use another WeMos or etc.

## 3 Steps

1. Upload ESPTestFramework firmware to NodeMCU (add you WiFi ssid&pwd)
2. Connect you device to NodeMCU, turn on NodeMCU. Web server runs.
3. Write Python test script and run it.

## Actions

### ping
Check link
```
api.ping()
```
What ESP do: 
Return: 'pong'

## DIO

### pinMode
Set pin mode as you usually do on C.
```
api.pinMode(pin, mode) 
```
What ESP do: 
```pinMode(pin, mode)```
Return: 'OK'

### digitalRead
Read DIO pin
```
api.digitalRead(pin) 
```
What ESP do: 
```digitalRead(pin)```
Return: 1 or 0 

### digitalWrite
Write value to DIO pin
```
api.digitalWrite(pin, value) 
```
What ESP do: 
```digitalWrite(pin, value)```
Return: 'OK'

## i2c communication

### Start
```
api.i2c_begin() 
```
What ESP do: 
```Wire.begin(SDA, SCL)```
Return: 'OK'

or set your pins
```
api.i2c_begin(sda_pin, scl_pin) 
```
```Wire.begin(sda_pin, scl_pin)```

### Set clock speed
```
api.i2c_setClock(value) 
```
What ESP do: 
```Wire.setClock(value)```
Return: 'OK'

### Set stretch limit
```
api.i2c_setClockStretchLimit(stretch) 
```
What ESP do: 
```Wire.setClockStretchLimit(stretch)```
Return: 'OK'

### Send & receive message

slave_address - address of i2c slave device
message - string
response_length - home many bytes will read after send

```
ret = api.i2c_ask(slave_address, message, response_length) 
```

What ESP do: 
```

Wire.beginTransmission(slave_address)

LOOP
	Wire.write(arr[i])

Wire.endTransmission()

LOOP 
	Wire.requestFrom(address, 1)
	Wire.read()
```
Return: 
code 200: response
code 500: error message


### ESP Firmware 

Based on https://github.com/me-no-dev/ESPAsyncWebServer


## Examples

## Blynk

Blynk NodeMCU onboard LED
```
from ESPTestFramework import ESPTestFramework, LOW, HIGH, INPUT, OUTPUT

api = ESPTestFramework(host)

pin = LED_BUILTIN_AUX

api.pinMode(pin, OUTPUT)
api.digitalWrite(pin, LOW)

assert api.digitalRead(pin) == LOW

api.delay(1000)
api.digitalWrite(pin, HIGH)

assert api.digitalRead(pin) == HIGH
```

## Check button pressed
```
api = ESPTestFramework(host)

api.pinMode(D5, INPUT_PULLUP)
assert api.wait_digital(D5, LOW, 3.0), "Button wasn't pressed"
```

## Check button pressed
```
api = ESPTestFramework(host)

api.pinMode(D5, INPUT_PULLUP)
assert api.wait_digital(D5, LOW, 3.0), "Button wasn't pressed"
```

## i2c communication
```
from ESPTestFramework import ESPTestFramework

api = ESPTestFramework(host)

api.i2c_begin(D3, D4)   #  call Wire.begin(D3, D4)

message = 'M'           #  send 1 byte '4D'
answer_len = 1          #  read 1 byte in the answer

ret = api.i2c_ask(address, message, answer_len)

assert ord(ret[0]) == 1, 'response not 1'
```

## Unpack binary structures

```
from ESPTestFramework import ESPTestFramework
from ESPTestFramework.utils import DataStruct

api = ESPTestFramework(host)

api.i2c_begin(D3, D4)

fields = [  ('version',      'B'),  # unsigned char
            ('value_uint16', 'H'),  # unsigned short
            ('value_uint32', 'L'),  # unsigned long
        ]

header_len = DataStruct.calcsize(fields)

ret = api.i2c_ask(12, 'A', header_len)

header = DataStruct(fields, ret)

print header.version
print header.value_uint16
print header.value_uint32
```

## ToDo

1. AVR ISP programmer by ESP to upload firmwares
