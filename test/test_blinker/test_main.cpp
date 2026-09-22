#include <unity.h>

#include <memory>

#include "blinker.h"
#include "fake_led_driver.h"

/*
Ритмы светодиода: периоды, смена режима, ручной цвет стенда.

Ритмы - из docs/architecture.md, раздел о светодиоде. Проверяется и то,
что в драйвер уходит только смена цвета: WS2812 на каждый show() занимает
линию, и слать ему одно и то же сотни раз в секунду незачем.
*/

using Pattern = Blinker::Pattern;

namespace {

std::unique_ptr<FakeLedDriver> drv;
std::unique_ptr<Blinker> blink;

void run_until(uint32_t until) {
    for (; drv->now < until; drv->now += 10) blink->loop(drv->now);
}

} // namespace

// Blinker держит ссылку на драйвер: пересоздаём оба на каждый тест
void setUp() {
    drv = std::make_unique<FakeLedDriver>();
    blink = std::make_unique<Blinker>(*drv);
}

void tearDown() {
    blink.reset();
    drv.reset();
}

void test_first_loop_shows_immediately_and_lit() {
    blink->set(Rgb::blue(), Pattern::Slow);
    run_until(10);
    TEST_ASSERT_EQUAL(1, drv->log.size());
    TEST_ASSERT_TRUE(drv->log[0].color == Rgb::blue());
}

void test_slow_is_one_second_on_one_off() {
    blink->set(Rgb::blue(), Pattern::Slow);
    run_until(4500);
    const auto on = drv->on_times();
    const auto off = drv->off_times();
    TEST_ASSERT_EQUAL(3, on.size());
    TEST_ASSERT_EQUAL_UINT32(0, on[0]);
    TEST_ASSERT_EQUAL_UINT32(2000, on[1]);
    TEST_ASSERT_EQUAL_UINT32(4000, on[2]);
    TEST_ASSERT_EQUAL_UINT32(1000, off[0]);
    TEST_ASSERT_EQUAL_UINT32(3000, off[1]);
}

void test_fast_is_quarter_second() {
    blink->set(Rgb::red(), Pattern::Fast);
    run_until(1100);
    const auto on = drv->on_times();
    TEST_ASSERT_EQUAL(3, on.size());
    TEST_ASSERT_EQUAL_UINT32(500, on[1]);
    TEST_ASSERT_EQUAL_UINT32(250, drv->off_times()[0]);
}

void test_heartbeat_dark_100ms_every_3s() {
    blink->set(Rgb::green(), Pattern::Heartbeat);
    run_until(6500);
    const auto off = drv->off_times();
    const auto on = drv->on_times();
    TEST_ASSERT_EQUAL(2, off.size());
    TEST_ASSERT_EQUAL_UINT32(2900, off[0]);
    TEST_ASSERT_EQUAL_UINT32(5900, off[1]);
    TEST_ASSERT_EQUAL_UINT32(3000, on[1]);
}

void test_solid_sends_once() {
    blink->set(Rgb::blue(), Pattern::Solid);
    run_until(10000);
    TEST_ASSERT_EQUAL(1, drv->log.size());
}

void test_off_is_dark() {
    blink->set(Rgb::blue(), Pattern::Off);
    run_until(3000);
    TEST_ASSERT_EQUAL(1, drv->log.size());
    TEST_ASSERT_TRUE(drv->log[0].color.dark());
}

void test_same_set_does_not_restart_rhythm() {
    blink->set(Rgb::blue(), Pattern::Slow);
    run_until(900);
    blink->set(Rgb::blue(), Pattern::Slow); // статус повторяется каждый loop()
    run_until(1100);
    TEST_ASSERT_EQUAL(1, drv->off_times().size());
    TEST_ASSERT_EQUAL_UINT32(1000, drv->off_times()[0]);
}

void test_new_mode_starts_lit_at_once() {
    blink->set(Rgb::blue(), Pattern::Slow);
    run_until(1500); // сейчас темно
    blink->set(Rgb::green(), Pattern::Heartbeat);
    run_until(1510);
    TEST_ASSERT_TRUE(drv->log.back().color == Rgb::green());
    TEST_ASSERT_EQUAL_UINT32(1500, drv->log.back().at);
}

void test_hold_overrides_rhythm_and_release_returns_it() {
    blink->set(Rgb::blue(), Pattern::Slow);
    run_until(100);
    const Rgb bench{10, 20, 30};
    blink->hold(bench);
    run_until(5000);
    TEST_ASSERT_EQUAL(2, drv->log.size()); // синий, затем цвет стенда - и всё
    TEST_ASSERT_TRUE(drv->log[1].color == bench);

    blink->release();
    run_until(5010);
    TEST_ASSERT_FALSE(drv->log.back().color == bench);
}

void test_status_changes_while_held_are_not_shown() {
    blink->hold(Rgb::red());
    run_until(100);
    blink->set(Rgb::green(), Pattern::Heartbeat);
    run_until(4000);
    TEST_ASSERT_EQUAL(1, drv->log.size());
}

void test_refresh_resends_same_color() {
    blink->set(Rgb::blue(), Pattern::Solid);
    run_until(100);
    blink->refresh();
    run_until(200);
    TEST_ASSERT_EQUAL(2, drv->log.size());
}

void test_rhythm_survives_millis_wrap() {
    drv->now = 0xFFFFFFFFu - 1500;
    blink->set(Rgb::blue(), Pattern::Slow);
    blink->loop(drv->now);
    const uint32_t start = drv->now;
    for (uint32_t t = 0; t < 4000; t += 10) blink->loop(drv->now = start + t);
    TEST_ASSERT_EQUAL(2, drv->on_times().size());
    TEST_ASSERT_EQUAL_UINT32(start + 2000, drv->on_times()[1]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_first_loop_shows_immediately_and_lit);
    RUN_TEST(test_slow_is_one_second_on_one_off);
    RUN_TEST(test_fast_is_quarter_second);
    RUN_TEST(test_heartbeat_dark_100ms_every_3s);
    RUN_TEST(test_solid_sends_once);
    RUN_TEST(test_off_is_dark);
    RUN_TEST(test_same_set_does_not_restart_rhythm);
    RUN_TEST(test_new_mode_starts_lit_at_once);
    RUN_TEST(test_hold_overrides_rhythm_and_release_returns_it);
    RUN_TEST(test_status_changes_while_held_are_not_shown);
    RUN_TEST(test_refresh_resends_same_color);
    RUN_TEST(test_rhythm_survives_millis_wrap);
    return UNITY_END();
}
