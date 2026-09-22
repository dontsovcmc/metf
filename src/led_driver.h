#pragma once

/*
Светодиод, которым мигает Blinker: одна точка одного цвета.

Интерфейс без Arduino, чтобы Blinker проверялся на хосте с поддельным
драйвером. Настоящие - RgbLedDriver (WS2812 через FastLED) и GpioLedDriver
(обычный светодиод на выводе).
*/

#include <cstdint>

struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    bool operator==(const Rgb &o) const { return r == o.r && g == o.g && b == o.b; }
    bool operator!=(const Rgb &o) const { return !(*this == o); }
    bool dark() const { return r == 0 && g == 0 && b == 0; }

    static constexpr Rgb off() { return Rgb{}; }
    static constexpr Rgb red() { return Rgb{255, 0, 0}; }
    static constexpr Rgb green() { return Rgb{0, 255, 0}; }
    static constexpr Rgb blue() { return Rgb{0, 0, 255}; }
};

class LedDriver {
public:
    LedDriver() = default;
    LedDriver(const LedDriver &) = delete;
    LedDriver &operator=(const LedDriver &) = delete;
    LedDriver(LedDriver &&) = delete;
    LedDriver &operator=(LedDriver &&) = delete;
    virtual ~LedDriver() = default;

    // Зажечь цвет (чёрный - погасить). Зовётся только при смене цвета.
    virtual void show(Rgb color) = 0;
};
