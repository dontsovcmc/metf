#include <Arduino.h>
#ifdef ESP32
#include <WiFi.h>
#include <AsyncTCP.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#endif
#include <ESPAsyncWebServer.h>

#include "Wire.h"

#include "logging.h"
#include "utils.h"
#include "AsyncSerialBuffer.h"

#define VALUE_TO_STRING(x) #x
#define VALUE(x) VALUE_TO_STRING(x)
#define VAR_NAME_VALUE(var) #var "=" VALUE(var)

AsyncWebServer server(80);
AsyncSerialBuffer asb;

// RGB LED Support
#ifdef ESP32
#include <FastLED.h>

#define RGB_MAX_LEDS 10  // Maximum supported LEDs (configurable)
#define RGB_DEFAULT_PIN 8  // Default GPIO pin for RGB LED (configurable)

static CRGB rgb_leds[RGB_MAX_LEDS];  // Pre-allocated LED array
static uint8_t rgb_num_leds = 0;     // Actual number of LEDs initialized
static uint8_t rgb_pin = 0;          // GPIO pin for LED data line
static bool rgb_initialized = false; // Initialization state flag
static uint8_t rgb_brightness = 255; // Current brightness (0-255)
#endif 

const char* PARAM_PIN = "pin";
const char* PARAM_VALUE = "value";
const char* PARAM_MSEC = "msec";
const char* PARAM_MODE = "mode";
const char* PARAM_INVERT  = "invert";
const char* PARAM_ACTION = "action";
const char* PARAM_SDA_PIN = "sda_pin";
const char* PARAM_SCL_PIN = "scl_pin";
const char* PARAM_ADDRESS = "address";
const char* PARAM_HEXSTRING = "hexstring";
const char* PARAM_LEN = "len";
const char* PARAM_RESPONSE = "response";
const char* PARAM_BAUDRATE = "baudrate";
const char* PARAM_NUMBER = "number";    // For RGB LED count


#define DEFAULT_BAUDRATE 115200
static unsigned long current_baud = DEFAULT_BAUDRATE;

static const uint32_t kAllowedBauds[] = {
  300, 1200, 2400, 4800, 9600, 19200, 38400,
  57600, 74880, 115200, 230400, 250000, 460800, 921600
};

bool isAllowedBaud(uint32_t b) {
  for (auto v : kAllowedBauds) if (v == b) return true;
  return false;
}

void notFound (AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
}

enum api_error_t {
    NO_GET_PARAM, 
    NO_FORM_PARAM,
    INCORRECT_VALUE
};

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
// Helper: Parse 6-character hex color string to RGB components
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

// Helper: Initialize or reinitialize RGB LED strip
// Returns: true if successful, false if parameters invalid
bool rgbBegin(uint8_t pin, uint8_t num_leds, String& error_msg) {
    // Validate LED count
    if (num_leds < 1 || num_leds > RGB_MAX_LEDS) {
        error_msg = "Number of LEDs must be 1-" + String(RGB_MAX_LEDS);
        return false;
    }

    // Store configuration
    rgb_num_leds = num_leds;
    rgb_pin = pin;

    FastLED.clear();
    
    // Clear LED array
    memset(rgb_leds, 0, sizeof(rgb_leds));
    
    // Initialize FastLED with default pin
    // Note: Pin is ignored from parameter, uses compile-time RGB_DEFAULT_PIN instead
    FastLED.addLeds<WS2812B, RGB_DEFAULT_PIN, GRB>(rgb_leds, num_leds);

    FastLED.setBrightness(rgb_brightness);

    // Initialize all LEDs to off
    LOCK();
    FastLED.show();
    UNLOCK();

    rgb_initialized = true;
    LOG_INFO("RGB initialized: pin=" << pin << " leds=" << num_leds);

    return true;
}
#endif

void setup() {
    LOG_BEGIN(115200);
    LOG_INFO("");
    LOG_INFO("Welcome to ESP Test Framework. Have a nice tests!");

    WiFi.mode(WIFI_STA);
    WiFi.begin(VALUE(SSID_NAME), VALUE(SSID_PASS));
    if (WiFi.waitForConnectResult() != WL_CONNECTED) {
        LOG_ERROR("WiFi Failed!");
        return;
    }

    LOG_INFO("IP Address: " << WiFi.localIP());

    // GET request to <IP>/ping
    server.on("/ping", HTTP_GET, [](AsyncWebServerRequest *request){
        LOG_INFO("GET /ping");
        request->send(200, "text/plain", "pong");
    });

    // POST request to <IP>/pinMode
    // pin=<number>
    // mode=<INPUT,OUTPUT,INPUT_PULLUP> integer constants
    server.on("/pinMode", HTTP_POST, [](AsyncWebServerRequest *request) {
        /*int headers = request->headers();
        int i;
        for(i=0;i<headers;i++){
            AsyncWebHeader* h = request->getHeader(i);
            Serial.printf("HEADER[%s]: %s\n", h->name().c_str(), h->value().c_str());
        } */
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
        uint32_t nb = DEFAULT_BAUDRATE;
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
            if (!request->hasParam(PARAM_PIN, true)) {
                response_400(request, NO_FORM_PARAM, PARAM_PIN);
                return;
            }
            if (!request->hasParam(PARAM_NUMBER, true)) {
                response_400(request, NO_FORM_PARAM, PARAM_NUMBER);
                return;
            }

            uint8_t pin = request->getParam(PARAM_PIN, true)->value().toInt();
            uint8_t num = request->getParam(PARAM_NUMBER, true)->value().toInt();

            String error_msg;
            if (!rgbBegin(pin, num, error_msg)) {
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
            for (uint8_t i = 0; i < rgb_num_leds; i++) {
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
#endif

    // GET request to <IP>/version
    // read framework version
    server.on("/version", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", METF_VERSION);
    });

    server.onNotFound(notFound);

    server.begin();
}

void loop() {
    while (Serial.available() > 0) {
        asb.pushChar((char)Serial.read());
    }
}