#pragma once

/*
Сеть METF целиком: подключение, портал настройки и светодиод, который
показывает режим.

Фасад, единственное место, где изолированные части встречаются: состояние
WifiLink здесь переводится в цвет и ритм Blinker, здесь выбирается драйвер
светодиода под плату, здесь живёт маршрут /rgb, через который стенд
забирает светодиод себе. main.cpp видит только begin() и loop().

Светодиод:
  синий медленно (1/1 с)       старт и подключение
  синий горит                  своя точка поднята, ждём настройки
  зелёный, гаснет раз в 3 с    в сети
  красный часто (250/250 мс)   сеть пропала, переподключаюсь
  красный медленно (1/1 с)     ошибка железа: точка или флеш
*/

#include <ESPAsyncWebServer.h>

#include <atomic>
#include <memory>

#include "blinker.h"
#include "timing.h"
#include "led_driver.h"
#include "wifi_link.h"
#include "wifi_portal.h"

class Connectivity {
public:
    struct Config {
        WifiLink::Config wifi;
        uint8_t status_brightness = 24; // WS2812 на полной яркости слепит
    };

    explicit Connectivity(const Config &cfg);

    Connectivity(const Connectivity &) = delete;
    Connectivity &operator=(const Connectivity &) = delete;
    Connectivity(Connectivity &&) = delete;
    Connectivity &operator=(Connectivity &&) = delete;
    ~Connectivity() = default;

    // Из setup(): радио, светодиод, маршруты портала и /rgb, onNotFound
    void begin(AsyncWebServer &server);

    // Из loop()
    void loop();

private:
    void show_status();
    void attach_rgb(AsyncWebServer &server);

    Config cfg_;
    WifiLink link_;
    WifiPortal portal_;
    std::unique_ptr<LedDriver> led_; // под плату; без светодиода - пустышка
    Blinker blinker_;                // объявлен после led_: держит ссылку на него

    // Яркость, которую стенд назначил через /rgb; у статуса своя
    std::atomic<uint8_t> manual_brightness_{255};

    uint32_t tick_at_ = 0;
};
