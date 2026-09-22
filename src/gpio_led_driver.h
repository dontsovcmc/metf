#pragma once

/*
Обычный светодиод на выводе (NodeMCU: GPIO 2, горит от нуля). Цвета у него
нет: любой не чёрный цвет - «горит».
*/

#include <Arduino.h>

#include "led_driver.h"

class GpioLedDriver : public LedDriver {
public:
    GpioLedDriver(uint8_t pin, bool active_low) : pin_(pin), active_low_(active_low) {}

    void begin() {
        pinMode(pin_, OUTPUT);
        show(Rgb::off());
    }

    void show(Rgb color) override {
        const bool on = !color.dark();
        digitalWrite(pin_, on != active_low_ ? HIGH : LOW);
    }

private:
    uint8_t pin_;
    bool active_low_;
};
