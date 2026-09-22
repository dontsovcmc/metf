#include <unity.h>

#include "fake_radio.h"
#include "wifi_policy.h"

/*
Тайминги политики подключения на виртуальных часах.

Каждый тест - один сценарий из жизни стенда: плату перенесли к другому
роутеру, роутер сменил канал, отключили свет и роутер грузится дольше платы,
человек настраивает сеть с телефона. Числа в проверках - из docs/wifi.md,
таблица таймингов; меняется таблица - меняются и они.
*/

using Action = WifiPolicy::Action;
using State = WifiPolicy::State;

namespace {

WifiPolicy policy;
FakeRadio radio;
FakeClock clock_;

// Плата была в сети, пара канал/BSSID сохранена
void bring_online() {
    radio.has_fast = true;
    radio.fast_channel = radio.router_channel;
    clock_.run_until(policy, radio, 10000);
    TEST_ASSERT_TRUE(policy.state() == State::Online);
    radio.log.clear();
}

} // namespace

void setUp() {
    policy = WifiPolicy();
    radio = FakeRadio();
    clock_ = FakeClock();
}

void tearDown() {}

// ---------------------------------------------------------------- старт

void test_first_attempt_waits_five_seconds() {
    clock_.run_until(policy, radio, 4900);
    TEST_ASSERT_TRUE(radio.log.empty());
    TEST_ASSERT_TRUE(policy.state() == State::Starting);

    clock_.run_until(policy, radio, 5100);
    TEST_ASSERT_EQUAL(1, radio.attempt_times().size());
    TEST_ASSERT_EQUAL_UINT32(5000, radio.attempt_times()[0]);
}

void test_no_router_two_attempts_then_ap_at_25s() {
    radio.router_up = false;
    clock_.run_until(policy, radio, 30000);

    const auto at = radio.attempt_times();
    TEST_ASSERT_EQUAL(2, at.size());
    TEST_ASSERT_EQUAL_UINT32(5000, at[0]);
    TEST_ASSERT_EQUAL_UINT32(15000, at[1]);

    const auto ap = radio.times_of(Action::StartAp);
    TEST_ASSERT_EQUAL(1, ap.size());
    TEST_ASSERT_EQUAL_UINT32(25000, ap[0]);
    TEST_ASSERT_TRUE(policy.state() == State::Ap);
}

void test_saved_pair_first_fast_then_scan() {
    radio.router_up = false;
    radio.has_fast = true;
    radio.fast_channel = 6;
    clock_.run_until(policy, radio, 26000);

    TEST_ASSERT_EQUAL(1, radio.times_of(Action::AttemptFast).size());
    TEST_ASSERT_EQUAL_UINT32(5000, radio.times_of(Action::AttemptFast)[0]);
    TEST_ASSERT_EQUAL(1, radio.times_of(Action::AttemptScan).size());
    TEST_ASSERT_EQUAL_UINT32(15000, radio.times_of(Action::AttemptScan)[0]);
}

void test_without_pair_both_attempts_scan() {
    radio.router_up = false;
    clock_.run_until(policy, radio, 26000);
    TEST_ASSERT_EQUAL(0, radio.times_of(Action::AttemptFast).size());
    TEST_ASSERT_EQUAL(2, radio.times_of(Action::AttemptScan).size());
}

void test_fast_connect_succeeds_in_first_attempt() {
    radio.has_fast = true;
    radio.fast_channel = 6;
    clock_.run_until(policy, radio, 9000);
    TEST_ASSERT_TRUE(policy.state() == State::Online);
    TEST_ASSERT_EQUAL(1, radio.attempt_times().size());
    TEST_ASSERT_EQUAL(0, radio.times_of(Action::StartAp).size());
}

void test_router_moved_channel_found_by_scan_and_pair_updated() {
    radio.has_fast = true;
    radio.fast_channel = 6;
    radio.router_channel = 11;
    clock_.run_until(policy, radio, 20000);

    TEST_ASSERT_TRUE(policy.state() == State::Online);
    TEST_ASSERT_EQUAL(1, radio.times_of(Action::AttemptFast).size());
    TEST_ASSERT_EQUAL(1, radio.times_of(Action::AttemptScan).size());
    TEST_ASSERT_EQUAL_UINT8(11, radio.fast_channel);
    TEST_ASSERT_EQUAL(0, radio.times_of(Action::StartAp).size());
}

void test_no_creds_ap_at_once_and_no_attempts() {
    radio.has_creds = false;
    clock_.run_until(policy, radio, 200000);
    const auto ap = radio.times_of(Action::StartAp);
    TEST_ASSERT_EQUAL(1, ap.size());
    TEST_ASSERT_EQUAL_UINT32(0, ap[0]);
    TEST_ASSERT_TRUE(radio.attempt_times().empty());
}

// ---------------------------------------------------------------- точка поднята

void test_probe_every_minute_while_ap_is_empty() {
    radio.router_up = false;
    clock_.run_until(policy, radio, 150000);
    const auto at = radio.attempt_times();
    TEST_ASSERT_EQUAL(4, at.size()); // 5, 15, затем 85 и 145
    TEST_ASSERT_EQUAL_UINT32(85000, at[2]);
    TEST_ASSERT_EQUAL_UINT32(145000, at[3]);
    TEST_ASSERT_EQUAL(0, radio.times_of(Action::AttemptFast).size());
}

void test_probes_scan_even_with_saved_pair() {
    radio.router_up = false;
    radio.has_fast = true;
    radio.fast_channel = 6;
    clock_.run_until(policy, radio, 150000);
    // быстрая - только первая попытка после включения
    TEST_ASSERT_EQUAL(1, radio.times_of(Action::AttemptFast).size());
}

void test_power_cut_router_boots_later_board_returns_by_probe() {
    // свет дали: плата стартует сразу, роутер грузится 90 с
    radio.router_up = false;
    clock_.run_until(policy, radio, 90000);
    TEST_ASSERT_TRUE(policy.state() == State::Ap);

    radio.router_up = true;
    clock_.run_until(policy, radio, 150000);
    TEST_ASSERT_TRUE(policy.state() == State::Online);
    // и гасит ненужную точку
    TEST_ASSERT_EQUAL(1, radio.times_of(Action::StopAp).size());
    TEST_ASSERT_FALSE(radio.ap_up);
}

void test_client_on_ap_blocks_probes() {
    radio.router_up = false;
    clock_.run_until(policy, radio, 30000);
    radio.ap_clients = 1; // телефон подключился и открыл страницу
    policy.portal_activity(clock_.now);

    clock_.run_until(policy, radio, 300000);
    TEST_ASSERT_EQUAL(2, radio.attempt_times().size());
}

void test_silent_client_stops_blocking_after_ten_minutes() {
    radio.router_up = false;
    clock_.run_until(policy, radio, 30000);
    radio.ap_clients = 1; // подключился, ничего не делает

    clock_.run_until(policy, radio, 30000 + 590000);
    TEST_ASSERT_EQUAL(2, radio.attempt_times().size());

    clock_.run_until(policy, radio, 30000 + 700000);
    TEST_ASSERT_TRUE(radio.attempt_times().size() > 2);
}

void test_new_creds_attempt_at_once_even_with_client() {
    radio.router_up = false;
    clock_.run_until(policy, radio, 30000);
    radio.ap_clients = 1;
    policy.portal_activity(clock_.now);

    radio.router_up = true; // человек ввёл правильную сеть
    policy.request_connect();
    const uint32_t asked = clock_.now;
    clock_.run_for(policy, radio, 200);

    const auto at = radio.attempt_times();
    TEST_ASSERT_EQUAL(3, at.size());
    TEST_ASSERT_UINT32_WITHIN(100, asked, at[2]);
    TEST_ASSERT_TRUE(policy.state() == State::Connecting);

    clock_.run_for(policy, radio, 5000);
    TEST_ASSERT_TRUE(policy.state() == State::Online);
}

void test_success_with_client_keeps_ap_for_a_minute() {
    radio.router_up = false;
    clock_.run_until(policy, radio, 30000);
    radio.ap_clients = 1;
    policy.portal_activity(clock_.now);
    radio.router_up = true;
    policy.request_connect();

    clock_.run_for(policy, radio, 4000); // подключились
    TEST_ASSERT_TRUE(policy.state() == State::Online);
    const uint32_t online = clock_.now;
    TEST_ASSERT_TRUE(radio.ap_up);

    clock_.run_until(policy, radio, online + 55000);
    TEST_ASSERT_TRUE(radio.ap_up);
    clock_.run_until(policy, radio, online + 61000);
    TEST_ASSERT_FALSE(radio.ap_up);
}

void test_wrong_new_creds_bring_ap_back_after_one_round() {
    radio.router_up = false;
    clock_.run_until(policy, radio, 30000);
    policy.request_connect(); // сеть с ошибкой в пароле - та же тишина
    const uint32_t asked = clock_.now;
    clock_.run_for(policy, radio, 21000);
    const auto ap = radio.times_of(Action::StartAp);
    TEST_ASSERT_EQUAL(2, ap.size());
    TEST_ASSERT_UINT32_WITHIN(200, asked + 20000, ap[1]);
}

void test_request_ap_raises_ap_at_once() {
    bring_online();
    policy.request_ap();
    clock_.run_for(policy, radio, 200);
    TEST_ASSERT_EQUAL(1, radio.times_of(Action::StartAp).size());
}

void test_ap_start_retried_when_it_did_not_come_up() {
    radio.router_up = false;
    clock_.run_until(policy, radio, 26000);
    radio.ap_up = false; // softAP() не удался
    clock_.run_until(policy, radio, 26000 + 10500);
    TEST_ASSERT_EQUAL(2, radio.times_of(Action::StartAp).size());
}

// ---------------------------------------------------------------- потеря сети

void test_lost_network_reconnects_without_ap_for_short_outage() {
    bring_online();
    radio.router_up = false;
    const uint32_t lost = clock_.now;
    clock_.run_until(policy, radio, lost + 60000);
    TEST_ASSERT_TRUE(policy.state() == State::Lost);

    radio.router_up = true;
    clock_.run_until(policy, radio, lost + 100000);
    TEST_ASSERT_TRUE(policy.state() == State::Online);
    TEST_ASSERT_EQUAL(0, radio.times_of(Action::StartAp).size());
}

void test_lost_network_first_attempt_is_immediate() {
    bring_online();
    radio.router_up = false;
    const uint32_t lost = clock_.now;
    clock_.run_for(policy, radio, 300);
    TEST_ASSERT_EQUAL(1, radio.attempt_times().size());
    TEST_ASSERT_UINT32_WITHIN(200, lost, radio.attempt_times()[0]);
}

void test_lost_network_ap_after_two_minutes() {
    bring_online();
    radio.router_up = false;
    const uint32_t lost = clock_.now;
    clock_.run_until(policy, radio, lost + 119000);
    TEST_ASSERT_EQUAL(0, radio.times_of(Action::StartAp).size());

    clock_.run_until(policy, radio, lost + 131000);
    const auto ap = radio.times_of(Action::StartAp);
    TEST_ASSERT_EQUAL(1, ap.size());
    TEST_ASSERT_UINT32_WITHIN(10000, lost + 125000, ap[0]); // 120..130 с
    TEST_ASSERT_TRUE(policy.state() == State::Ap);
}

void test_stale_pair_forgotten_after_two_failed_fast_attempts() {
    bring_online();
    radio.router_up = false;
    const uint32_t lost = clock_.now;
    clock_.run_until(policy, radio, lost + 119000);
    TEST_ASSERT_EQUAL(2, radio.times_of(Action::AttemptFast).size());
    TEST_ASSERT_EQUAL(1, radio.times_of(Action::ForgetFast).size());
    TEST_ASSERT_FALSE(radio.has_fast);
}

void test_radio_restart_every_fourth_failed_round() {
    WifiPolicy::Config cfg;
    cfg.lost_ap_after_ms = 1000000; // держим плату без AP, чтобы набрать раунды
    policy = WifiPolicy(cfg);
    bring_online();
    radio.router_up = false;
    clock_.run_for(policy, radio, 4 * 25000 + 1000);
    TEST_ASSERT_EQUAL(1, radio.times_of(Action::RestartRadio).size());
}

void test_millis_wrap_during_outage() {
    bring_online();
    clock_.now = 0xFFFFFFFFu - 30000; // скачок часов вперёд, пока в сети
    clock_.run_for(policy, radio, 1000);
    radio.router_up = false;
    const uint32_t lost = clock_.now;
    clock_.run_for(policy, radio, 131000); // переполнение посреди отсчёта
    const auto ap = radio.times_of(Action::StartAp);
    TEST_ASSERT_EQUAL(1, ap.size());
    TEST_ASSERT_UINT32_WITHIN(10000, lost + 125000, ap[0]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_first_attempt_waits_five_seconds);
    RUN_TEST(test_no_router_two_attempts_then_ap_at_25s);
    RUN_TEST(test_saved_pair_first_fast_then_scan);
    RUN_TEST(test_without_pair_both_attempts_scan);
    RUN_TEST(test_fast_connect_succeeds_in_first_attempt);
    RUN_TEST(test_router_moved_channel_found_by_scan_and_pair_updated);
    RUN_TEST(test_no_creds_ap_at_once_and_no_attempts);
    RUN_TEST(test_probe_every_minute_while_ap_is_empty);
    RUN_TEST(test_probes_scan_even_with_saved_pair);
    RUN_TEST(test_power_cut_router_boots_later_board_returns_by_probe);
    RUN_TEST(test_client_on_ap_blocks_probes);
    RUN_TEST(test_silent_client_stops_blocking_after_ten_minutes);
    RUN_TEST(test_new_creds_attempt_at_once_even_with_client);
    RUN_TEST(test_success_with_client_keeps_ap_for_a_minute);
    RUN_TEST(test_wrong_new_creds_bring_ap_back_after_one_round);
    RUN_TEST(test_request_ap_raises_ap_at_once);
    RUN_TEST(test_ap_start_retried_when_it_did_not_come_up);
    RUN_TEST(test_lost_network_reconnects_without_ap_for_short_outage);
    RUN_TEST(test_lost_network_first_attempt_is_immediate);
    RUN_TEST(test_lost_network_ap_after_two_minutes);
    RUN_TEST(test_stale_pair_forgotten_after_two_failed_fast_attempts);
    RUN_TEST(test_radio_restart_every_fourth_failed_round);
    RUN_TEST(test_millis_wrap_during_outage);
    return UNITY_END();
}
