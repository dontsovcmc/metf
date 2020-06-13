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

AsyncWebServer server(80);

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

#define VERSION 1

void notFound(AsyncWebServerRequest *request) {
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

void setup() {
    LOG_BEGIN(115200);
    LOG_INFO("");
    LOG_INFO("Welcome to ESP Test Framework. Have a nice tests!");

    WiFi.mode(WIFI_STA);
    WiFi.begin(SSID_NAME, SSID_PASS);
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
            AsyncWebParameter *param = request->getParam(i);
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
            LOG_INFO("Wire.setClockStretchLimit(" << stretch << ")");

            Wire.setClockStretchLimit(stretch);

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

    // GET request to <IP>/version 
    // read framework version
    server.on("/version", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", String(VERSION));
    });

    server.onNotFound(notFound);

    server.begin();


    
}

void loop() {
}