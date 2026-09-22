#pragma once

/*
Адресный светодиод WS2812 (на ESP32-C6 SuperMini - GPIO 8) через FastLED.

Вывод - параметр шаблона: FastLED.addLeds принимает его только так. Яркость
применяется на следующем show(), поэтому set_brightness() можно звать из
обработчика HTTP, а сам светодиод трогает только тот, кто зовёт show()
(Blinker из loop()).
*/

#include <FastLED.h>

#include <atomic>

#include "led_driver.h"

template <uint8_t PIN> class RgbLedDriver : public LedDriver {
public:
    explicit RgbLedDriver(uint8_t brightness) : brightness_(brightness) {}

    // Один раз, из setup(). Повторный addLeds завёл бы второй контроллер.
    void begin() {
        FastLED.addLeds<WS2812B, PIN, GRB>(&led_, 1);
        FastLED.setBrightness(brightness_.load());
    }

    void set_brightness(uint8_t value) { brightness_.store(value); }
    uint8_t brightness() const { return brightness_.load(); }

    void show(Rgb color) override {
        led_ = CRGB(color.r, color.g, color.b);
        FastLED.setBrightness(brightness_.load());
        FastLED.show();
    }

private:
    CRGB led_;
    std::atomic<uint8_t> brightness_;
};
