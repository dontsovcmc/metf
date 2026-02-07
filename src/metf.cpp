#include "metf.h"
#include "logging.h"
#include "utils.h"

#define VALUE_TO_STRING(x) #x
#define VALUE(x) VALUE_TO_STRING(x)

// Parameter constants
extern const char* PARAM_PIN;
extern const char* PARAM_VALUE;
extern const char* PARAM_MODE;
extern const char* PARAM_ACTION;
extern const char* PARAM_SDA_PIN;
extern const char* PARAM_SCL_PIN;
extern const char* PARAM_ADDRESS;
extern const char* PARAM_HEXSTRING;
extern const char* PARAM_LEN;
extern const char* PARAM_RESPONSE;
extern const char* PARAM_BAUDRATE;

extern bool isAllowedBaud(uint32_t b);

#include "Wire.h"

// HTTP response helpers
void notFound(AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
}

void response_400(AsyncWebServerRequest *request, api_error_t err, const String &name)
{
    switch(err) {
        case NO_GET_PARAM:
            request->send(400, "text/plain", "parameter \'" + name + "\' not found");
            break;
        case NO_FORM_PARAM:
            request->send(400, "text/plain", "post form parameter \'" + name + "\' not found");
            break;
        case INCORRECT_VALUE:
            request->send(400, "text/plain", "parameter \'" + name + "\' is incorrect");
            break;
    }
}

void response_500(AsyncWebServerRequest *request, const String &what)
{
    request->send(500, "text/plain", what);
}

#ifdef ESP32
#ifdef RGB_DEFAULT_PIN
// RGB Helper: Parse 6-character hex color string to RGB components
// Input: "FF0000" or "ff0000" (red)
// Returns: true if valid, sets r, g, b parameters
bool parseHexColor(const String& hex, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (hex.length() != 6) {
        return false;
    }

    // Validate all characters are hex
    for (char c : hex) {
        if (!isxdigit(c)) {
            return false;
        }
    }

    // Parse RGB components using existing utility functions
    r = (hexCharToInt(hex[0]) << 4) | hexCharToInt(hex[1]);
    g = (hexCharToInt(hex[2]) << 4) | hexCharToInt(hex[3]);
    b = (hexCharToInt(hex[4]) << 4) | hexCharToInt(hex[5]);

    return true;
}

// RGB Helper: Initialize or reinitialize RGB LED strip
// Returns: true if successful, false if parameters invalid
bool rgbBegin(String& error_msg) {
    FastLED.clear();
    // Clear LED array
    memset(rgb_leds, 0, sizeof(CRGB) * RGB_NUMBER);

    // Initialize FastLED with default pin
    // Note: Pin is ignored from parameter, uses compile-time RGB_DEFAULT_PIN instead
    FastLED.addLeds<WS2812B, RGB_DEFAULT_PIN, GRB>(rgb_leds, RGB_NUMBER);

    FastLED.setBrightness(rgb_brightness);

    // Initialize all LEDs to off
    LOCK();
    FastLED.show();
    UNLOCK();

    rgb_initialized = true;
    LOG_INFO("RGB initialized: pin=" << RGB_DEFAULT_PIN << " leds=" << RGB_NUMBER);

    return true;
}
#endif // RGB_DEFAULT_PIN
#endif // ESP32

void setupMETFEndpoints() {
    // GET request to <IP>/ping
    server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *request){
        LOG_INFO("GET /ping");
        request->send(200, "text/plain", "pong");
    });

    // POST request to <IP>/pinMode
    // pin=<number>
    // mode=<INPUT,OUTPUT,INPUT_PULLUP> integer constants
    server.on("/pinMode", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!request->hasParam(PARAM_PIN, true)) {
            response_400(request, NO_FORM_PARAM, PARAM_PIN);
            return;
        }
        if (!request->hasParam(PARAM_MODE, true)) {
            response_400(request, NO_FORM_PARAM, PARAM_MODE);
            return;
        }

        uint8_t pin = request->getParam(PARAM_PIN, true)->value().toInt();
        uint8_t mode = request->getParam(PARAM_MODE, true)->value().toInt();

        pinMode(pin, mode);
        request->send(200, "text/plain", "OK");
    });

    // Send a GET request to <IP>/digitalRead?pin=<number>
    server.on("/digitalRead", HTTP_GET, [] (AsyncWebServerRequest *request) {

        if (!request->hasParam(PARAM_PIN)) {
            response_400(request, NO_GET_PARAM, PARAM_PIN);
            return;
        }

        uint8_t pin = request->getParam(PARAM_PIN)->value().toInt();

        //TODO check pin 0 - x
        if (digitalRead(pin) == HIGH) {
            request->send(200, "text/plain", "1");
        } else {
            request->send(200, "text/plain", "0");
        }
    });

    // POST request to <IP>/digitalWrite
    // form fields:
    // pin=<number>
    // value=<HIGH, LOW> constants
    server.on("/digitalWrite", HTTP_POST, [](AsyncWebServerRequest *request){

        if (!request->hasParam(PARAM_PIN, true)) {
            response_400(request, NO_FORM_PARAM, PARAM_PIN);
            return;
        }

        if (!request->hasParam(PARAM_VALUE, true)) {
            response_400(request, NO_FORM_PARAM, PARAM_VALUE);
            return;
        }

        uint8_t pin = request->getParam(PARAM_PIN, true)->value().toInt();
        uint8_t value = request->getParam(PARAM_VALUE, true)->value().toInt();

        digitalWrite(pin, value);
        request->send(200, "text/plain", "OK");
    });

    server.on("/i2c", HTTP_POST, [](AsyncWebServerRequest *request){
        String action, hexstring;
        uint8_t sda_pin = SDA, scl_pin = SCL, b, address, len;
        int err, i;

        if (!request->hasParam(PARAM_ACTION, true)) {
            response_400(request, NO_FORM_PARAM, PARAM_ACTION);
            return;
        }

        LOG_INFO("POST /i2c");
        for (size_t i = 0; i < request->params(); i++) {
            const AsyncWebParameter *param = request->getParam(i);
            if (param) {
                LOG_INFO("  " << param->name() << "=" << param->value());
            }
        }

        action = request->getParam(PARAM_ACTION, true)->value();

        if (action == "begin") {
            if (request->hasParam(PARAM_SDA_PIN, true)
                && request->hasParam(PARAM_SCL_PIN, true)) {
                sda_pin = request->getParam(PARAM_SDA_PIN, true)->value().toInt();
                scl_pin = request->getParam(PARAM_SCL_PIN, true)->value().toInt();
            }

            LOG_INFO("Wire begin SDA=" << sda_pin << " SCL=" << scl_pin);
            Wire.begin(sda_pin, scl_pin);

        } else if (action == "setClock") {
            if (!request->hasParam(PARAM_VALUE, true)) {
                response_400(request, NO_FORM_PARAM, PARAM_VALUE);
                return;
            }

            uint32_t clock = request->getParam(PARAM_VALUE, true)->value().toInt();
            LOG_INFO("Wire.setClock(" << clock << ")");

            Wire.setClock(clock);

        } else if (action == "setClockStretchLimit") {

            if (!request->hasParam(PARAM_VALUE, true)) {
                response_400(request, NO_FORM_PARAM, PARAM_VALUE);
                return;
            }

            uint32_t stretch = request->getParam(PARAM_VALUE, true)->value().toInt();

            #ifdef ESP8266
            // ESP8266: setClockStretchLimit takes microseconds
            LOG_INFO("Wire.setClockStretchLimit(" << stretch << " us)");
            Wire.setClockStretchLimit(stretch);
            #elif defined(ESP32)
            // ESP32/ESP32-C6: setTimeOut takes milliseconds
            // Convert microseconds to milliseconds, minimum 1000ms
            uint32_t timeout_ms = stretch / 1000;
            if (timeout_ms < 1000) {
                timeout_ms = 1000; // Minimum threshold for ESP32
            }
            LOG_INFO("Wire.setTimeOut(" << timeout_ms << " ms) [converted from " << stretch << " us]");
            Wire.setTimeOut(timeout_ms);
            #endif

        } else if (action == "ask") {

            if (!request->hasParam(PARAM_ADDRESS, true)) {
                response_400(request, NO_FORM_PARAM, PARAM_ADDRESS);
                return;
            }
            if (!request->hasParam(PARAM_HEXSTRING, true)) {
                response_400(request, NO_FORM_PARAM, PARAM_HEXSTRING);
                return;
            }
            if (!request->hasParam(PARAM_RESPONSE, true)) {
                response_400(request, NO_FORM_PARAM, PARAM_RESPONSE);
                return;
            }

            uint8_t response_len = request->getParam(PARAM_RESPONSE, true)->value().toInt();
            address = request->getParam(PARAM_ADDRESS, true)->value().toInt();
            hexstring = request->getParam(PARAM_HEXSTRING, true)->value();

            uint8_t arr[hexstring.length()];  //2

            len = hexText2AsciiArray(hexstring, arr, hexstring.length());

            if (len == 0) {
                response_400(request, INCORRECT_VALUE, PARAM_HEXSTRING);
                return;
            }

            Wire.beginTransmission(address);
            for(int i=0; i<len; i++) {
                LOG_DEBUG("i2c > " << String(arr[i], 16));
                if (Wire.write(arr[i]) != 1) {
                    Wire.endTransmission();
                    response_500(request, "i2c write error");
                    return;
                }
            }

            err = Wire.endTransmission();
            if (err != 0) {
                response_500(request, "i2c end transmission error " + String(err));
                /* https://www.arduino.cc/en/Reference/WireEndTransmission
                0:success
                1:data too long to fit in transmit buffer
                2:received NACK on transmit of address
                3:received NACK on transmit of data
                4:other error
                */
                return;
            }

            LOG_INFO("Wire send: " << hexstring << " " << len << " bytes to device " << address);
            delay(1); // Дадим время подумать

            hexstring.clear();
            hexstring.reserve(response_len * 2 + 1);   //TODO is null terminator needed?

            i = 0;
            while (i < response_len) {
                if (Wire.requestFrom(address, (uint8_t)1) != (uint8_t)1) {
                    response_500(request, "i2c read timeout. Received: " + hexstring);
                    return;
                }
                b = Wire.read();
                LOG_DEBUG("i2c < " << String(b, 16));

                hexstring += intToHexChar(b >> 4);
                hexstring += intToHexChar(b & 0x0F);
                i++;
            }


            LOG_INFO("Received: " << hexstring);

            request->send(200, "text/plain", hexstring);
            return;

        } else if (action == "flush") {
            Wire.flush();
            request->send(200, "text/plain", "OK");

        } else {
            response_400(request, INCORRECT_VALUE, PARAM_ACTION);
        }
        request->send(200, "text/plain", "OK");
    });

    // POST request to <IP>/serial
    // baudrate=<baudrate>
    server.on("/serial", HTTP_POST, [](AsyncWebServerRequest* request){
        String out;
        uint32_t nb = 115200; // DEFAULT_BAUDRATE
        if (request->hasParam(PARAM_BAUDRATE)) {
            String sv = request->getParam(PARAM_BAUDRATE)->value();
            nb = (uint32_t) sv.toInt();
        }

        if (nb == 0 || !isAllowedBaud(nb)) {
            request->send(400, "text/plain; charset=utf-8", "Invalid speed");
            return;
        }

        if (nb != current_baud) {
            // Короткая критическая секция: останавливаем приём и переключаем UART
            LOCK();
            Serial.end();
            Serial.begin(nb);
            current_baud = nb;
            UNLOCK();

            out = "Set " + String(nb) + " baudrate";
        } else {
            out = "Baudrate is " + String(nb);
        }

        if (request->hasParam("flush")) {
            String fv = request->getParam("flush")->value();
            if (fv == "1") {
                asb.flush();
                out += ", flush buffer";
            }
        }

        request->send(200, "text/plain; charset=utf-8", out);
    });


    server.on("/read", HTTP_GET, [](AsyncWebServerRequest *request){

        AsyncResponseStream* res = request->beginResponseStream("text/plain; charset=utf-8");
        // Слить накопленные строки без добавления разделителей
        asb.drain_to(*res);
        request->send(res);
    });

#ifdef ESP32
#ifdef RGB_DEFAULT_PIN
    // POST request to <IP>/rgb
    // action=begin&pin=<gpio>&number=<count>
    // action=brightness&value=<0-255>
    // action=color&value=<RRGGBB>
    server.on("/rgb", HTTP_POST, [](AsyncWebServerRequest *request){
        LOG_INFO("POST /rgb");

        // Log all parameters for debugging
        for (size_t i = 0; i < request->params(); i++) {
            const AsyncWebParameter *param = request->getParam(i);
            if (param) {
                LOG_INFO("  " << param->name() << "=" << param->value());
            }
        }

        // Action parameter is required
        if (!request->hasParam(PARAM_ACTION, true)) {
            response_400(request, NO_FORM_PARAM, PARAM_ACTION);
            return;
        }

        String action = request->getParam(PARAM_ACTION, true)->value();

        // Handle 'begin' action
        if (action == "begin") {
            String error_msg;
            if (!rgbBegin(error_msg)) {
                response_500(request, error_msg);
                return;
            }

            request->send(200, "text/plain", "OK");
            return;
        }

        // For brightness and color actions, RGB must be initialized first
        if (!rgb_initialized) {
            response_500(request, "RGB not initialized. Call action=begin first");
            return;
        }

        // Handle 'brightness' action
        if (action == "brightness") {
            if (!request->hasParam(PARAM_VALUE, true)) {
                response_400(request, NO_FORM_PARAM, PARAM_VALUE);
                return;
            }

            int brightness = request->getParam(PARAM_VALUE, true)->value().toInt();

            // Validate range
            if (brightness < 0 || brightness > 255) {
                response_400(request, INCORRECT_VALUE, PARAM_VALUE);
                return;
            }

            rgb_brightness = (uint8_t)brightness;
            FastLED.setBrightness(rgb_brightness);

            // Update LEDs with thread safety
            LOCK();
            FastLED.show();
            UNLOCK();

            LOG_INFO("RGB brightness set to " << brightness);
            request->send(200, "text/plain", "OK");
            return;
        }

        // Handle 'color' action
        if (action == "color") {
            if (!request->hasParam(PARAM_VALUE, true)) {
                response_400(request, NO_FORM_PARAM, PARAM_VALUE);
                return;
            }

            String hex_color = request->getParam(PARAM_VALUE, true)->value();
            uint8_t r, g, b;

            if (!parseHexColor(hex_color, r, g, b)) {
                response_400(request, INCORRECT_VALUE, PARAM_VALUE);
                return;
            }

            // Set all LEDs to the same color
            for (uint8_t i = 0; i < RGB_NUMBER; i++) {
                rgb_leds[i] = CRGB(r, g, b);
            }

            // Update LEDs with thread safety
            LOCK();
            FastLED.show();
            UNLOCK();

            LOG_INFO("RGB color set to #" << hex_color);
            request->send(200, "text/plain", "OK");
            return;
        }

        // Unknown action
        response_400(request, INCORRECT_VALUE, PARAM_ACTION);
    });
#endif // RGB_DEFAULT_PIN
#endif // ESP32

    // GET request to <IP>/version
    // read framework version
    server.on("/version", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", METF_VERSION);
    });

    // GET request to <IP>/hwversion
    // read firmware version
    server.on("/hwversion", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", FIRMWARE_VERSION);
    });
}
