#include "wifi_policy.h"

using Action = WifiPolicy::Action;
using State = WifiPolicy::State;

Action WifiPolicy::tick(uint32_t now, const Facts &f) {
    // Новый клиент на точке - такой же признак жизни, как запрос к порталу:
    // человек только подключился и ещё не успел открыть страницу
    if (f.ap_clients > prev_clients_) portal_activity(now);
    prev_clients_ = f.ap_clients;

    if (pending_ap_) {
        pending_ap_ = false;
        started_ = true;
        return enter_ap(now, true);
    }

    if (pending_connect_) {
        pending_connect_ = false;
        started_ = true;
        // Новая сеть: отказ в ней - повод сразу показать точку, а не ждать
        // две минуты, как для сети, которая уже работала
        ever_online_ = false;
        fast_disabled_ = false;
        fast_fails_ = 0;
        failed_rounds_ = 0;
        attempt_in_round_ = 0;
        if (!f.has_creds) return enter_ap(now, false);
        state_ = State::Connecting;
        return start_attempt(now, f, true);
    }

    if (!started_) {
        started_ = !f.has_creds || now >= cfg_.start_delay_ms;
        if (!started_) return Action::None;
        if (!f.has_creds) return enter_ap(now, false);
        return start_attempt(now, f, true);
    }

    if (f.connected) {
        if (state_ != State::Online) {
            state_ = State::Online;
            online_since_ = now;
            attempting_ = false;
            attempt_in_round_ = 0;
            ever_online_ = true;
            fast_disabled_ = false;
            fast_fails_ = 0;
            failed_rounds_ = 0;
            failed_attempts_ = 0;
            // Пара, которой только что подключились, рабочая: снимаем и
            // отложенный приказ её забыть, иначе он сработает в следующий
            // разрыв и сотрёт проверенный канал
            forget_pending_ = false;
        }
        // Точку гасим, как только она никому не нужна. Клиенту даём минуту:
        // он настраивал сеть и должен успеть увидеть новый адрес платы.
        // Точку, поднятую по просьбе, не гасим: её поднимали, чтобы к ней
        // подключились, а на это нужно время.
        if (f.ap_up && !keep_manual_ap(now, f) &&
            (f.ap_clients == 0 || elapsed(now, online_since_, cfg_.ap_stop_grace_ms)))
            return Action::StopAp;
        return Action::None;
    }

    if (state_ == State::Online) {
        state_ = State::Lost;
        lost_since_ = now;
        attempt_in_round_ = 0;
        return start_attempt(now, f, true);
    }

    if (attempting_) {
        if (!elapsed(now, attempt_started_, cfg_.attempt_timeout_ms)) return Action::None;
        return on_attempt_timeout(now, f);
    }

    if (forget_pending_) {
        forget_pending_ = false;
        return Action::ForgetFast;
    }

    if (!f.has_creds && state_ != State::Ap) return enter_ap(now, false);

    if (state_ == State::Ap) {
        if (!f.ap_up && elapsed(now, ap_requested_at_, cfg_.ap_retry_ms)) {
            ap_requested_at_ = now;
            return Action::StartAp;
        }
        if (!f.has_creds || portal_busy(now, f)) return Action::None;
        if (!elapsed(now, wait_since_, cfg_.probe_period_ms)) return Action::None;
        // Проба при поднятой точке - всегда со сканом: роутер после
        // перезагрузки мог выбрать другой канал
        return start_attempt(now, f, false);
    }

    if (state_ == State::Lost && elapsed(now, lost_since_, cfg_.lost_ap_after_ms))
        return enter_ap(now, false);

    if (!elapsed(now, wait_since_, wait_ms_)) return Action::None;
    return start_attempt(now, f, true);
}

Action WifiPolicy::start_attempt(uint32_t now, const Facts &f, bool allow_fast) {
    // Быстрая - только первая в раунде: если роутер ушёл на другой канал,
    // вторая попытка сканом его найдёт
    attempt_fast_ = allow_fast && f.has_fast && !fast_disabled_ && attempt_in_round_ == 0;
    attempting_ = true;
    attempt_started_ = now;
    wait_since_ = now;
    if (state_ == State::Starting) state_ = State::Connecting;
    return attempt_fast_ ? Action::AttemptFast : Action::AttemptScan;
}

Action WifiPolicy::on_attempt_timeout(uint32_t now, const Facts &f) {
    attempting_ = false;
    failed_attempts_++;

    if (attempt_fast_ && !fast_disabled_ && ++fast_fails_ >= cfg_.forget_fast_after) {
        // Пара канал/BSSID устарела: до следующего успеха её не пробуем, а
        // сохранённую стираем, чтобы и после перезагрузки не тратить на неё раунд
        fast_disabled_ = true;
        forget_pending_ = true;
    }

    // Проба не удалась - ждём следующую. Период пробы отсчитан от её начала
    // (start_attempt), чтобы пробы шли ровно раз в минуту, а не раз в 70 с
    if (state_ == State::Ap) return Action::None;

    // Вторая попытка раунда - сразу, без паузы
    if (++attempt_in_round_ < cfg_.round_attempts) return start_attempt(now, f, true);

    attempt_in_round_ = 0;
    wait_since_ = now;
    wait_ms_ = 0;
    failed_rounds_++;

    if (!ever_online_) return enter_ap(now, false);
    if (state_ == State::Lost && elapsed(now, lost_since_, cfg_.lost_ap_after_ms))
        return enter_ap(now, false);

    wait_ms_ = cfg_.lost_pause_ms;
    // Перезапуск радио гасит и точку доступа, а поднять её обратно некому:
    // пока она в эфире, лечим радио только попытками
    if (!f.ap_up && cfg_.radio_restart_every != 0 &&
        failed_rounds_ % cfg_.radio_restart_every == 0)
        return Action::RestartRadio;
    return Action::None;
}

Action WifiPolicy::enter_ap(uint32_t now, bool manual) {
    state_ = State::Ap;
    attempting_ = false;
    attempt_in_round_ = 0;
    ap_requested_at_ = now;
    wait_since_ = now;
    ap_manual_ = manual;
    if (manual) portal_activity(now); // человек идёт к точке, дадим ему время
    return Action::StartAp;
}

/*
Точку, поднятую кнопкой или командой, держим, пока к ней кто-то подключён и
ещё ap_manual_ms после последнего признака жизни. Иначе она гаснет через
доли секунды: плата в сети, клиентов нет - и общее правило её тушит.
*/
bool WifiPolicy::keep_manual_ap(uint32_t now, const Facts &f) const {
    if (!ap_manual_) return false;
    if (f.ap_clients > 0) return true;
    return !elapsed(now, busy_mark_, cfg_.ap_manual_ms);
}

bool WifiPolicy::portal_busy(uint32_t now, const Facts &f) const {
    return f.ap_clients > 0 && busy_mark_valid_ && !elapsed(now, busy_mark_, cfg_.portal_idle_ms);
}
