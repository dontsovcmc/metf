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

#include "AsyncSerialBuffer.h"
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
    static constexpr int kPulseSlots = 8;

    struct PulseSlot {
        Ticker timer;
        volatile bool busy = false;
        uint8_t pin = 0;
    };

    void pulse_end(int slot);
    int pulse_slot_of(uint8_t pin) const;   // слот, занятый этим выводом, или -1
    int pulse_slot_free() const;

    void on_pin_mode(AsyncWebServerRequest *request);
    void on_digital_read(AsyncWebServerRequest *request);
    void on_digital_write(AsyncWebServerRequest *request);
    void on_pulse(AsyncWebServerRequest *request);
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
    PulseSlot pulse_[kPulseSlots];
#ifdef ESP32
    NtpServer ntp_;
#endif
};
