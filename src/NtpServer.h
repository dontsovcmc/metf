#ifndef _METF_NTP_SERVER_h
#define _METF_NTP_SERVER_h

/*
NTP-сервер стенда: плата отдаёт устройству-под-тестом то время, которое
назначил тест.

Своих часов у платы нет - ни батарейного RTC, ни интернета. Это не недостаток,
а условие задачи: стенд должен работать без сети, а тест должен уметь назначать
время, в том числе заведомо узнаваемое. Момент задаётся по HTTP, дальше ход
времени отсчитывается от millis().

Слушатель не поднимается сам: плата, отвечающая на NTP в чужой сети без
спроса, - сюрприз, которого никто не заказывал. Нужен - `POST /ntp action=start`.

Только ESP32: AsyncUDP входит в ядро arduino-esp32, у ESP8266 его нет.
*/

#ifdef ESP32

#include <Arduino.h>
#include <AsyncUDP.h>

#include "ntp_packet.h"

class NtpServer
{
public:
    NtpServer();

    /*
    Назначить время и начать отвечать.

    @param epoch unix-время «сейчас»
    @param port  порт, 123 по умолчанию
    @return false, если порт занять не удалось
    */
    bool begin(uint32_t epoch, uint16_t port = NTP_PORT);

    /* Перестать слушать. Клиент получит ICMP «порт недоступен». */
    void stop();

    /* Переставить часы, не трогая слушателя. */
    void set_time(uint32_t epoch);

    /*
    Молча выбрасывать запросы, продолжая слушать.

    Отличается от stop() тем, что клиент не получит отказа и будет ждать
    таймаут - так выглядит недоступный сервер в интернете, а не закрытый порт.
    */
    void set_drop(bool on);

    bool running() const { return _running; }
    bool dropping() const { return _drop; }

    /* Текущее время платы: назначенное плюс прошедшее с того момента. */
    uint32_t now_epoch() const;
    uint32_t now_msec() const;

    /* Счётчики для проверок. Читаются из другого потока, берём под замком. */
    struct Stat
    {
        uint32_t requests;   // сколько пакетов пришло
        uint32_t replies;    // на сколько ответили
        uint32_t dropped;    // сколько выбросили: молчим или время не задано
        uint32_t ignored;    // сколько не похожи на запрос клиента
        uint32_t last_epoch; // время в последнем ответе
        IPAddress last_client;
    };

    Stat stat() const;

private:
    void handle(AsyncUDPPacket &packet);

    /*
    Держим объект указателем, потому что освободить порт иначе нечем:
    AsyncUDP::close() только разрывает связь с удалённым адресом, а привязку
    и обработчик снимает лишь деструктор (udp_recv(NULL) + udp_remove).
    С одним объектом «остановленный» сервер продолжал бы отвечать.
    */
    AsyncUDP *_udp;
    bool _running;
    bool _drop;
    bool _time_set;

    // Точка отсчёта: назначенное время и millis() того момента
    uint32_t _base_epoch;
    uint32_t _base_millis;

    Stat _stat;

    /*
    Обработчик AsyncUDP выполняется в задаче lwIP, а счётчики читает задача
    веб-сервера. Тот же приём, что в AsyncSerialBuffer.
    */
    mutable portMUX_TYPE _mux;
};

#endif // ESP32

#endif
