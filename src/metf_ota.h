#pragma once

#ifdef ESP32

#include <Arduino.h>
#include <ArduinoOTA.h>

// Extern declaration for global variable from main.cpp
extern String ap_name;

// OTA functions
void setupOTA();
void handleOTA();

#endif // ESP32
