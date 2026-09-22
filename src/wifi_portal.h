#pragma once

/*
Портал настройки сети: страница, API /wifi и всё, чтобы телефон сам
открыл страницу, подключившись к точке платы (captive portal).

Страница - обычная HTML-форма без JavaScript: captive-браузер iPhone
урезан, а форма работает везде. Статус подключения - на той же странице,
она обновляет себя сама (meta refresh), пока плата подключается.

О радио знает только через WifiLink, о светодиоде не знает ничего.
Маршруты и ответы - docs/api.md.
*/

#include <DNSServer.h>
#include <ESPAsyncWebServer.h>

#include "wifi_link.h"

class WifiPortal {
public:
    explicit WifiPortal(WifiLink &link) : link_(link) {}

    // /, GET и POST /wifi, редирект клиентам точки (onNotFound)
    void attach(AsyncWebServer &server);

    // Из loop(): DNS отвечает, пока поднята точка
    void loop();

private:
    // Клиенту точки - редирект на страницу, true; прочим - false
    bool handle_not_found(AsyncWebServerRequest *request);
    bool from_ap(AsyncWebServerRequest *request) const;
    void redirect_home(AsyncWebServerRequest *request) const;
    void on_page(AsyncWebServerRequest *request);
    void on_status(AsyncWebServerRequest *request);
    void on_command(AsyncWebServerRequest *request);

    WifiLink &link_;
    DNSServer dns_;
    bool dns_running_ = false;
};
