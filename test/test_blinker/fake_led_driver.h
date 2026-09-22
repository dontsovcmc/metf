#pragma once

/*
Поддельный светодиод для тестов Blinker: записывает, какой цвет и когда
ему велели зажечь. Время ставит тест - драйвер часов не знает.
*/

#include <cstdint>
#include <vector>

#include "led_driver.h"

class FakeLedDriver : public LedDriver {
public:
    struct Entry {
        uint32_t at;
        Rgb color;
    };

    uint32_t now = 0;    // выставляет тест перед Blinker::loop()
    std::vector<Entry> log;

    void show(Rgb color) override { log.push_back({now, color}); }

    // Моменты, когда светодиод загорался (из тёмного в цвет)
    std::vector<uint32_t> on_times() const {
        std::vector<uint32_t> out;
        bool was_lit = false;
        for (const auto &e : log) {
            if (!e.color.dark() && !was_lit) out.push_back(e.at);
            was_lit = !e.color.dark();
        }
        return out;
    }

    std::vector<uint32_t> off_times() const {
        std::vector<uint32_t> out;
        bool was_lit = false;
        for (const auto &e : log) {
            if (e.color.dark() && was_lit) out.push_back(e.at);
            was_lit = !e.color.dark();
        }
        return out;
    }
};
