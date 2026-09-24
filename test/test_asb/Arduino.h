#pragma once

/*
Заглушка Arduino.h для host-тестов кольца лога.

Кольцу от Arduino нужны ровно две вещи: `Print`, куда оно печатает строки, и
пара `noInterrupts`/`interrupts` вместо критической секции. Тащить ради них
настоящий фреймворк незачем - арифметика номеров строк проверяется на хосте за
миллисекунды, а на плате её проверять дорого и медленно.
*/

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

class Print {
public:
    virtual ~Print() = default;
    void print(const char *text) { out += text; }
    void print(char c) { out += c; }

    std::string out;
};

inline void noInterrupts() {}
inline void interrupts() {}
