#pragma once

/*
Адресный светодиод WS2812 (на ESP32-C6 SuperMini - GPIO 8) через RMT ядра.

Ни FastLED, ни rgbLedWrite(): первый на выдаче битов заслоняет радио, а
второй переинициализирует RMT на каждый показ. Здесь инициализация один раз в
begin(), дальше - асинхронная выдача 24 символов: контроллер выдаёт их сам,
задача не ждёт. Измерения - docs/wifi.md, «светодиод и отзывчивость».

Яркость масштабируется здесь: у RMT её нет.
*/

#include <Arduino.h>

#include <atomic>

#include "led_driver.h"

template <uint8_t PIN> class RgbLedDriver : public LedDriver {
public:
    explicit RgbLedDriver(uint8_t brightness) : brightness_(brightness) {}

    // 10 МГц: тик 0.1 мкс, в него укладываются T0H/T1H ленты
    void begin() override { ready_ = rmtInit(PIN, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000); }

    void set_brightness(uint8_t value) override { brightness_.store(value); }

    void show(Rgb color) override {
        if (!ready_) return;
        const uint16_t k = brightness_.load();
        const uint8_t grb[3] = {static_cast<uint8_t>(color.g * k / 255),
                                static_cast<uint8_t>(color.r * k / 255),
                                static_cast<uint8_t>(color.b * k / 255)};
        int i = 0;
        for (const uint8_t byte : grb) {
            for (int bit = 7; bit >= 0; bit--) {
                const bool one = (byte >> bit) & 1;
                bits_[i].level0 = 1;
                bits_[i].duration0 = one ? 8 : 4; // 0.8 или 0.4 мкс
                bits_[i].level1 = 0;
                bits_[i].duration1 = one ? 4 : 8;
                i++;
            }
        }
        // Асинхронно: ждать 30 мкс выдачи незачем, а следующий показ будет
        // не раньше чем через сотню миллисекунд
        rmtWriteAsync(PIN, bits_, RMT_SYMBOLS_OF(bits_));
    }

private:
    rmt_data_t bits_[24] = {};
    std::atomic<uint8_t> brightness_;
    bool ready_ = false;
};
