#ifdef ESP32

#include "metf_ota.h"
#include "logging.h"

// Setup ArduinoOTA
void setupOTA() {
    ArduinoOTA.setHostname(ap_name.c_str());

    ArduinoOTA.onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) {
            type = "sketch";
        } else { // U_SPIFFS
            type = "filesystem";
        }
        LOG_INFO("OTA Update Start: " << type);
    });

    ArduinoOTA.onEnd([]() {
        LOG_INFO("OTA Update Complete");
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        static unsigned int lastPercent = 0;
        unsigned int percent = (progress / (total / 100));
        if (percent != lastPercent && percent % 10 == 0) {
            LOG_INFO("OTA Progress: " << percent << "%");
            lastPercent = percent;
        }
    });

    ArduinoOTA.onError([](ota_error_t error) {
        LOG_ERROR("OTA Error[" << error << "]: ");
        if (error == OTA_AUTH_ERROR) LOG_ERROR("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) LOG_ERROR("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) LOG_ERROR("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) LOG_ERROR("Receive Failed");
        else if (error == OTA_END_ERROR) LOG_ERROR("End Failed");
    });

    ArduinoOTA.begin();
    LOG_INFO("ArduinoOTA started. Use PlatformIO 'Upload' over network");
}

// Handle OTA processing
void handleOTA() {
    ArduinoOTA.handle();
}

#endif // ESP32
