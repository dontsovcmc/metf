#pragma once

/*
Симулятор радио и роутера для тестов WifiPolicy.

Делает то же, что порт (WifiLink) на плате: исполняет действия политики и
сообщает ей факты. Мир устроен просто: роутер включён или нет и стоит на
каком-то канале; ассоциация занимает assoc_ms; быстрая попытка удаётся,
только если роутер на сохранённом канале. После успеха порт запоминает
канал - и симулятор тоже.
*/

#include <cstdint>
#include <vector>

#include "wifi_policy.h"

struct FakeRadio {
    using Action = WifiPolicy::Action;

    struct Entry {
        uint32_t at;
        Action action;
    };

    // мир
    bool router_up = true;
    uint8_t router_channel = 6;
    uint32_t assoc_ms = 3000;

    // плата
    bool has_creds = true;
    bool has_fast = false;
    uint8_t fast_channel = 0;
    bool connected = false;
    bool ap_up = false;
    uint8_t ap_clients = 0;

    bool attempting = false;
    bool attempt_fast = false;
    uint32_t attempt_at = 0;

    std::vector<Entry> log; // все действия, кроме None

    WifiPolicy::Facts facts() const {
        WifiPolicy::Facts f;
        f.has_creds = has_creds;
        f.has_fast = has_fast;
        f.connected = connected;
        f.ap_up = ap_up;
        f.ap_clients = ap_clients;
        return f;
    }

    void apply(uint32_t now, Action a) {
        if (a == Action::None) return;
        log.push_back({now, a});
        switch (a) {
        case Action::AttemptFast:
        case Action::AttemptScan:
            attempting = true;
            attempt_fast = a == Action::AttemptFast;
            attempt_at = now;
            break;
        case Action::StartAp:
            ap_up = true;
            break;
        case Action::StopAp:
            ap_up = false;
            ap_clients = 0;
            break;
        case Action::RestartRadio:
            attempting = false;
            break;
        case Action::ForgetFast:
            has_fast = false;
            fast_channel = 0;
            break;
        case Action::None:
            break;
        }
    }

    // Ход мира: закончилась ли ассоциация, не пропал ли роутер
    void step(uint32_t now) {
        if (!router_up) connected = false;
        if (attempting && static_cast<uint32_t>(now - attempt_at) >= assoc_ms) {
            attempting = false;
            const bool reachable = !attempt_fast || fast_channel == router_channel;
            if (router_up && reachable) {
                connected = true;
                has_fast = true;
                fast_channel = router_channel;
            }
        }
    }

    // Сколько раз и когда было действие
    std::vector<uint32_t> times_of(Action a) const {
        std::vector<uint32_t> out;
        for (const auto &e : log)
            if (e.action == a) out.push_back(e.at);
        return out;
    }

    std::vector<uint32_t> attempt_times() const {
        std::vector<uint32_t> out;
        for (const auto &e : log)
            if (e.action == Action::AttemptFast || e.action == Action::AttemptScan)
                out.push_back(e.at);
        return out;
    }
};

// Виртуальные часы: гонят политику и радио шагами по step_ms
struct FakeClock {
    uint32_t now = 0;
    uint32_t step_ms = 100;

    void run_until(WifiPolicy &p, FakeRadio &r, uint32_t until) {
        while (static_cast<int32_t>(until - now) > 0) {
            r.step(now);
            r.apply(now, p.tick(now, r.facts()));
            now += step_ms;
        }
    }

    void run_for(WifiPolicy &p, FakeRadio &r, uint32_t ms) { run_until(p, r, now + ms); }
};
