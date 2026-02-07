#include <Arduino.h>
#ifdef ESP32
#include <WiFi.h>
#include <AsyncTCP.h>
#include <Preferences.h>
#include <DNSServer.h>
#elif defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESPAsyncTCP.h>
#endif
#include <ESPAsyncWebServer.h>
#include "Wire.h"

#include "logging.h"
#include "utils.h"
#include "AsyncSerialBuffer.h"

// Module headers
#include "metf.h"
#ifdef ESP32
#include "metf_wifi.h"
#include "metf_ota.h"
#endif

#define VALUE_TO_STRING(x) #x
#define VALUE(x) VALUE_TO_STRING(x)
#define VAR_NAME_VALUE(var) #var "=" VALUE(var)

AsyncWebServer server(80);
AsyncSerialBuffer asb;

// WiFi Management for ESP32
#ifdef ESP32
Preferences preferences;
DNSServer dnsServer;
bool isAPMode = false;
String ap_name;  // AP name for OTA and portal
#define WIFI_SSID_KEY "wifi_ssid"
#define WIFI_PASS_KEY "wifi_pass"
#define PREFS_NAMESPACE "metf"
#endif

// RGB LED Support
#ifdef ESP32
#ifdef RGB_DEFAULT_PIN
#include <FastLED.h>

#ifndef RGB_NUMBER
#define RGB_NUMBER 1 // Maximum supported LEDs (configurable)
#endif

CRGB rgb_leds[RGB_NUMBER];  // Pre-allocated LED array
bool rgb_initialized = false; // Initialization state flag
uint8_t rgb_brightness = 255; // Current brightness (0-255)
#endif // RGB_DEFAULT_PIN
#endif // ESP32 

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
unsigned long current_baud = DEFAULT_BAUDRATE;

static const uint32_t kAllowedBauds[] = {
  300, 1200, 2400, 4800, 9600, 19200, 38400,
  57600, 74880, 115200, 230400, 250000, 460800, 921600
};

bool isAllowedBaud(uint32_t b) {
  for (auto v : kAllowedBauds) if (v == b) return true;
  return false;
}




void setup() {
    LOG_BEGIN(115200);
    LOG_INFO("");
    LOG_INFO("Welcome to ESP Test Framework. Have a nice tests!");

    // WiFi Configuration
    #ifdef ESP8266
    // ESP8266: Use hardcoded credentials
    WiFi.mode(WIFI_STA);
    WiFi.begin(VALUE(SSID_NAME), VALUE(SSID_PASS));
    if (WiFi.waitForConnectResult() != WL_CONNECTED) {
        LOG_ERROR("WiFi Failed!");
        return;
    }
    LOG_INFO("IP Address: " << WiFi.localIP());

    #elif defined(ESP32)
    // ESP32: Use WiFi module
    setupWiFi();
    #endif

    // Setup all endpoints
    setupMETFEndpoints();

    #ifdef ESP32
    setupWiFiEndpoints();
    setupOTA();
    #endif

    server.onNotFound(notFound);
    server.begin();
}

void loop() {
    #ifdef ESP32
    handleOTA();
    handleDNS();
    #endif

    // AsyncSerialBuffer processing
    while (Serial.available() > 0) {
        asb.pushChar((char)Serial.read());
    }
}