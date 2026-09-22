#pragma once

/*
Политика подключения к WiFi: когда пробовать, как (быстро или со сканом),
когда поднимать и гасить свою точку доступа.

Чистый C++ без Arduino: время и факты о радио приходят снаружи, решение
уходит наружу. Поэтому вся логика таймингов проверяется на хосте
(test/test_wifi_policy), а порт (WifiLink) только сообщает факты и исполняет.

Алгоритм и его обоснование - docs/wifi.md, раздел «Алгоритм».

  старт ─ 5 с ─ раунд (2 попытки по 10 с) ─┬ успех ─► в сети
                                           └ неудача ─► AP
  в сети ─ потеря ─► раунды с паузой 5 с; через 2 мин без сети ─► AP
  AP: раз в 60 с, если к AP никто не подключён, - попытка со сканом

Все сравнения времени - беззнаковой разностью: millis() переполняется
через 49 дней, а плата живёт дольше.
*/

#include <cstdint>

class WifiPolicy {
public:
    enum class State : uint8_t {
        Starting,   // пауза после включения
        Connecting, // идёт попытка, своей точки нет
        Online,     // в сети
        Lost,       // была сеть и пропала, переподключаемся без AP
        Ap          // своя точка доступа поднята, ждём настройки
    };

    enum class Action : uint8_t {
        None,
        AttemptFast,  // подключиться на сохранённых канале и BSSID
        AttemptScan,  // подключиться с полным сканом
        StartAp,      // поднять свою точку (STA остаётся)
        StopAp,       // погасить свою точку
        RestartRadio, // WIFI_OFF и обратно: лекарство ESPHome от залипшего радио
        ForgetFast    // стереть сохранённые канал и BSSID
    };

    struct Config {
        uint32_t start_delay_ms = 5000;      // от включения до первой попытки
        uint32_t attempt_timeout_ms = 10000; // на одну попытку
        uint8_t round_attempts = 2;          // попыток в раунде
        uint32_t lost_pause_ms = 5000;       // между раундами после потери сети
        uint32_t lost_ap_after_ms = 120000;  // сколько без сети до подъёма AP
        uint32_t probe_period_ms = 60000;    // проба при поднятой AP
        uint32_t portal_idle_ms = 600000;    // клиент AP без запросов перестаёт мешать
        uint32_t ap_stop_grace_ms = 60000;   // AP живёт после успеха, пока клиент на ней
        uint32_t ap_manual_ms = 600000;      // точка, поднятая по просьбе, ждёт человека
        uint32_t ap_retry_ms = 10000;        // повтор StartAp, если точка не поднялась
        uint8_t forget_fast_after = 2;       // неудачных быстрых подряд до забывания пары
        uint8_t radio_restart_every = 4;     // каждый N-й неудачный раунд подряд
    };

    // Что порт видит в железе на этом шаге
    struct Facts {
        bool has_creds = false;  // есть SSID, к которому подключаться
        bool has_fast = false;   // сохранены канал и BSSID
        bool connected = false;  // есть IP
        bool ap_up = false;      // точка доступа поднята (по железу, не по флагу)
        uint8_t ap_clients = 0;  // станций на точке
    };

    WifiPolicy() = default;
    explicit WifiPolicy(const Config &cfg) : cfg_(cfg) {}

    // Шаг политики. Звать часто (из loop()); за шаг - не больше одного действия.
    Action tick(uint32_t now_ms, const Facts &f);

    // Новые креды: бросить всё и начать раунд сейчас, в обход пауз и клиентов AP
    void request_connect() { pending_connect_ = true; }

    // Поднять точку сейчас (кнопка, HTTP)
    void request_ap() { pending_ap_ = true; }

    // Человек на портале что-то сделал: пока он занят, пробы его не прерывают
    void portal_activity(uint32_t now_ms) {
        busy_mark_ = now_ms;
        busy_mark_valid_ = true;
    }

    State state() const { return state_; }
    bool attempting() const { return attempting_; }

    // Неудачных попыток с последнего подключения
    uint32_t failed_attempts() const { return failed_attempts_; }

private:
    Action start_attempt(uint32_t now, const Facts &f, bool allow_fast);
    Action on_attempt_timeout(uint32_t now, const Facts &f);
    Action enter_ap(uint32_t now, bool manual);
    bool keep_manual_ap(uint32_t now, const Facts &f) const;
    bool portal_busy(uint32_t now, const Facts &f) const;
    static bool elapsed(uint32_t now, uint32_t since, uint32_t period) {
        return static_cast<uint32_t>(now - since) >= period;
    }

    Config cfg_;
    State state_ = State::Starting;
    bool started_ = false;

    bool attempting_ = false;
    bool attempt_fast_ = false;
    uint32_t attempt_started_ = 0;
    uint8_t attempt_in_round_ = 0; // сколько попыток раунда уже сделано

    bool ever_online_ = false;     // с загрузки или со смены кредов
    bool fast_disabled_ = false;   // пара не сработала, до следующего успеха не пробуем
    bool forget_pending_ = false;
    uint8_t fast_fails_ = 0;
    uint8_t failed_rounds_ = 0;
    uint32_t failed_attempts_ = 0;

    uint32_t lost_since_ = 0;
    uint32_t wait_since_ = 0;      // отсчёт паузы до следующей попытки
    uint32_t wait_ms_ = 0;
    uint32_t online_since_ = 0;
    uint32_t ap_requested_at_ = 0;
    bool ap_manual_ = false; // точку просили кнопкой или по HTTP, а не политика

    uint8_t prev_clients_ = 0;
    uint32_t busy_mark_ = 0;       // последний признак жизни клиента AP
    bool busy_mark_valid_ = false;

    bool pending_connect_ = false;
    bool pending_ap_ = false;
};
