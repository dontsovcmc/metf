#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include "AsyncSerialBuffer.h"

#ifdef ESP32
#ifdef RGB_DEFAULT_PIN
#include <FastLED.h>
#endif
#endif

// Extern declarations for global variables from main.cpp
extern AsyncWebServer server;
extern AsyncSerialBuffer asb;
extern unsigned long current_baud;

#ifdef ESP32
#ifdef RGB_DEFAULT_PIN
extern CRGB rgb_leds[];
extern bool rgb_initialized;
extern uint8_t rgb_brightness;
#endif
#endif

// HTTP response helpers
enum api_error_t {
    NO_GET_PARAM,
    NO_FORM_PARAM,
    INCORRECT_VALUE
};

void notFound(AsyncWebServerRequest *request);
void response_400(AsyncWebServerRequest *request, api_error_t err, const String &name);
void response_500(AsyncWebServerRequest *request, const String &what);

// RGB helpers (ESP32 only)
#ifdef ESP32
#ifdef RGB_DEFAULT_PIN
bool parseHexColor(const String& hex, uint8_t& r, uint8_t& g, uint8_t& b);
bool rgbBegin(String& error_msg);
#endif
#endif

// Setup function to register all METF endpoints
void setupMETFEndpoints();
