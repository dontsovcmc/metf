#pragma once

/*
Маршруты стенда: всё, чем тест управляет испытуемым устройством.

GPIO и импульсы, I2C, UART испытуемого с кольцом лога, NTP-сервер (ESP32),
версия протокола. Сеть, портал и светодиод живут в Connectivity и сюда не входят.
Параметры и ответы каждого маршрута - docs/api.md.
*/

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <Ticker.h>

#include <atomic>

#include "AsyncSerialBuffer.h"
#include "wave.h"
#ifdef ESP32
#include "NtpServer.h"
#endif

class BenchRoutes {
public:
    // dut - порт, на котором говорит испытуемое устройство
    explicit BenchRoutes(HardwareSerial &dut);

    BenchRoutes(const BenchRoutes &) = delete;
    BenchRoutes &operator=(const BenchRoutes &) = delete;

    // Поднять UART испытуемого на скорости по умолчанию
    void begin();

    void attach(AsyncWebServer &server);

    // Из loop(): слить UART испытуемого в кольцо лога
    void loop();

private:
    /*
    Импульс отмеряет таймер ядра, а не обработчик запроса: callback сервера
    работает в задаче async_tcp, которая обслуживает все соединения платы, и
    delay() в ней останавливает их все разом (README библиотеки: «You can not
    use yield or delay or any function that uses them inside the callbacks»).
    Импульсы идут по нескольким выводам разом: стенд жмёт кнопку, пока по входу
    счётчика идёт серия. Занятым бывает вывод, а не плата, поэтому слотов
    несколько - по одному таймеру на каждый. Восемь с запасом: стенду хватает
    четырёх (кнопка, сброс, два входа).
    */
    static_assert(wave::kMaxLines == 8, "слотов ровно столько, сколько линий в пачке");
    static constexpr int kPulseSlots = wave::kMaxLines;

    /*
    Приёмный буфер UART испытуемого.

    Голова сеанса - самый плотный залп во всём логе устройства: полсотни строк
    за треть секунды, около 2,8 КБ. Умолчание ядра - 256 байт
    (`HardwareSerial.cpp`, `_rxBufferSize(256)`), а это на 115200 всего 22 мс:
    стоит loop() задержаться на радио дольше - и байты пропадают в драйвере,
    до кольца, где их не считает никто. Четыре килобайта держат 355 мс.
    */
    static constexpr size_t kRxBufferBytes = 4096;

    struct PulseSlot {
        Ticker timer;
        volatile bool busy = false;
        wave::Line line{};          // заказанная линия: вывод, уровень, участки
        uint8_t index = 0;          // участок, который выставит следующий тик
        /*
        Фактические моменты фронтов в аптайме платы: участков n, моментов n + 1
        (последний - отпускание вывода). По ним стенд утверждает, что подал, -
        заказанному интервалу верить нельзя, его искажает дорога запроса.
        */
        uint32_t marks[wave::kMaxEdges + 1] = {};
        uint8_t mark_count = 0;
        bool in_batch = false;      // участвовал в последней пачке: для /pulse/stat
    };

    /*
    Поставить счётчик переполнений драйвера на UART испытуемого.

    Отдельным методом по двум причинам. `HardwareSerial::end()` снимает
    обработчик (`_onReceiveErrorCB = NULL`), а смена скорости через /serial идёт
    именно через end()+begin() - без повторной постановки счётчик замолчал бы
    после первой же смены, и молчание было бы неотличимо от «переполнений не
    было». И звать это можно только вне LOCK(): обработчик создаёт задачу
    событий (`_createEventTask`), а LOCK() на ESP32 - спинлок с запретом
    прерываний; проверено на плате - она уходит в перезагрузку.
    */
    void watch_overruns();

    /*
    Очередной фронт линии: выставить уровень следующего участка или отпустить
    вывод, если участки кончились. Зовётся из таймера, поэтому вся работа -
    регистры GPIO и millis(): ни сети, ни delay.
    */
    void pulse_tick(int slot);
    void pulse_arm(int slot, uint32_t ms);
    void pulse_start(const wave::Batch &batch);
    // Проверить пачку, отказать или запустить и ответить распиской
    void pulse_answer(AsyncWebServerRequest *request, const wave::Batch &batch);
    int pulse_slot_of(uint8_t pin) const;   // слот, занятый этим выводом, или -1
    int pulse_slot_free() const;
    int pulse_slots_free() const;

    void on_pin_mode(AsyncWebServerRequest *request);
    void on_digital_read(AsyncWebServerRequest *request);
    void on_digital_write(AsyncWebServerRequest *request);
    void on_pulse(AsyncWebServerRequest *request);
    void on_pulse_stat(AsyncWebServerRequest *request);
    void on_i2c(AsyncWebServerRequest *request);
    void on_serial(AsyncWebServerRequest *request);
    void on_read_stat(AsyncWebServerRequest *request);
    void on_read(AsyncWebServerRequest *request);
#ifdef ESP32
    void on_ntp_stat(AsyncWebServerRequest *request);
    void on_ntp(AsyncWebServerRequest *request);
#endif

    HardwareSerial &dut_;
    AsyncSerialBuffer asb_;
    uint32_t baud_;
    // Считает драйвер в своей задаче, читает обработчик запроса - отсюда atomic
    std::atomic<uint32_t> overruns_{0};
    PulseSlot pulse_[kPulseSlots];
    uint32_t batch_start_ms_ = 0;   // общий старт последней пачки
#ifdef ESP32
    NtpServer ntp_;
#endif
};
