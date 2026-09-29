#pragma once

/*
Имя платы в сети: `metf.local`.

Адрес платы выдаёт DHCP, и он меняется сам по себе - при перезагрузке роутера,
при истечении аренды. Стенд, который ищет плату по адресу из своего файла, в
такой день не находит её вовсе и падает по таймауту, ничего не объяснив. Имя
переживает смену адреса, и спросить его умеет любая машина - отдельной
библиотеки для этого не нужно.

Запись поднимается, когда у платы появился адрес, и снимается, когда сеть
пропала: в записи лежит адрес, и оставленная от прошлого подключения она
уводила бы спрашивающего по старому.
*/

#include <Arduino.h>

class MdnsService {
public:
    // host - имя без `.local`; instance - как плата зовётся в списке служб
    void begin(const char *host, const char *instance, uint16_t port, const char *version);

    // Из loop(); online - есть ли сейчас адрес
    void loop(bool online);

private:
    void start();
    void stop();

    String host_;
    String instance_;
    String version_;
    uint16_t port_ = 80;
    bool up_ = false;
};
