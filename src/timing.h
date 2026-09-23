#pragma once

/*
Мелочи, нужные и в чистом коде, и в коде с Arduino.

Без Arduino: этот заголовок подключает WifiPolicy, который собирается на
хосте.
*/

#include <atomic>
#include <cstdint>

/*
Прошло ли period с момента since. Беззнаковая разность: millis()
переполняется через 49 дней, а плата живёт дольше, и сравнение
`now > since + period` на переполнении врёт.
*/
inline bool elapsed(uint32_t now, uint32_t since, uint32_t period) {
    return static_cast<uint32_t>(now - since) >= period;
}

/*
Снять флаг, поставленный другим потоком.

Не exchange(): на ESP8266 нет атомарных read-modify-write, и
__atomic_exchange_1 не линкуется. Флаг ставят снаружи, а снимает только
loop(), поэтому повторная постановка между load и store сливается с
первой - как и при exchange.
*/
inline bool take(std::atomic<bool> &flag) {
    if (!flag.load()) return false;
    flag.store(false);
    return true;
}
