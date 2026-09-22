#pragma once

/*
Мигалка: цвет и ритм -> включения и выключения светодиода.

Ничего не знает ни о сети, ни о сервере: кто-то снаружи говорит «синий,
медленно», Blinker отбивает ритм по часам, которые ему передают в loop().
Поэтому ритмы проверяются на хосте (test/test_blinker).

Ритм начинается с «горит»: смена режима видна сразу, а не через полпериода.

Ручной режим (hold) - цвет, выставленный снаружи мимо ритмов, например
стендом через /rgb. hold/release/refresh можно звать из другой задачи (из
обработчика HTTP): они только ставят атомарные флаги, а в драйвер цвет
уходит из loop(). Так светодиод трогает один поток.
*/

#include <atomic>
#include <cstdint>

#include "led_driver.h"

class Blinker {
public:
    enum class Pattern : uint8_t {
        Off,       // погашен
        Solid,     // горит
        Slow,      // 1 с горит, 1 с нет
        Fast,      // 250 мс горит, 250 мс нет
        Heartbeat  // горит, раз в 3 с гаснет на 100 мс: видно, что прошивка жива
    };

    explicit Blinker(LedDriver &driver) : driver_(driver) {}

    // Ритм статуса. Тот же цвет и ритм повторно ритм не сбивает.
    void set(Rgb color, Pattern pattern);

    // Ручной цвет поверх ритма; release() возвращает ритм
    void hold(Rgb color);
    void release();
    bool held() const { return held_.load(); }

    // Показать заново тот же цвет: драйвер поменял яркость
    void refresh() { refresh_.store(true); }

    // Из loop(): отправить в драйвер, если цвет сменился
    void loop(uint32_t now_ms);

    Pattern pattern() const { return pattern_; }
    Rgb color() const { return color_; }

private:
    bool lit(uint32_t now_ms) const; // горит ли ритм в этот момент

    static uint32_t pack(Rgb c) { return (uint32_t(c.r) << 16) | (uint32_t(c.g) << 8) | c.b; }
    static Rgb unpack(uint32_t v) {
        return Rgb{uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    }

    LedDriver &driver_;

    Rgb color_;
    Pattern pattern_ = Pattern::Off;
    uint32_t phase_start_ = 0;
    bool phase_valid_ = false; // начало ритма - на первом loop() после set()

    std::atomic<bool> held_{false};
    std::atomic<uint32_t> held_color_{0};
    std::atomic<bool> refresh_{false};

    Rgb shown_;
    bool shown_valid_ = false; // в драйвер ещё ничего не уходило
};
