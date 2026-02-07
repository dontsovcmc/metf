#pragma once

#ifdef ESP32

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>

#ifdef RGB_DEFAULT_PIN
#include <FastLED.h>
#endif

// Extern declarations for global variables from main.cpp
extern AsyncWebServer server;
extern Preferences preferences;
extern DNSServer dnsServer;
extern bool isAPMode;
extern String ap_name;

#ifdef RGB_DEFAULT_PIN
extern CRGB rgb_leds[];
#endif

// WiFi credentials keys
#define WIFI_SSID_KEY "wifi_ssid"
#define WIFI_PASS_KEY "wifi_pass"
#define PREFS_NAMESPACE "metf"

// WiFi management functions
bool connectToWiFi(const String& ssid, const String& password);
void startAPMode(const String& ap_name);
void setupWiFi();
void setupWiFiEndpoints();
void handleDNS();

#endif // ESP32
