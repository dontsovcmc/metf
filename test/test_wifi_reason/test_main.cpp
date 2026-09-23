#include <unity.h>

#include <cstring>
#include <initializer_list>

#include "wifi_reason.h"

/*
Код отказа ядра -> причина для человека.

Главное, что здесь проверяется, - правило раскладки, а не список кодов:
меньше 200 - это reason code из кадра точки доступа, и он значит только
«связь оборвалась» (кроме 15: рукопожатие о пароле); с 200 - коды самой
станции, и они авторитетны.

Нумерация у ESP32 и ESP8266 общая, поэтому ветвления по платформе нет.
*/

void setUp() {}
void tearDown() {}

namespace {

void check(int reason, WifiProblem expected, const char *key) {
    const WifiProblem got = wifi_problem(reason);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(expected), static_cast<int>(got),
                                  wifi_problem_key(got));
    TEST_ASSERT_EQUAL_STRING(key, wifi_problem_key(got));
}

} // namespace

void test_no_reason_is_no_problem() {
    check(0, WifiProblem::None, "none");
    TEST_ASSERT_EQUAL_STRING("", wifi_problem_text(WifiProblem::None));
}

// 202 AUTH_FAIL и 204 HANDSHAKE_TIMEOUT - то, что видит опечатавшийся
void test_station_says_the_password_was_refused() {
    check(202, WifiProblem::Password, "password");
    check(204, WifiProblem::Password, "password");
}

// 15 - кадр от точки, но о рукопожатии: единственный код ниже 200, по
// которому про пароль можно говорить уверенно
void test_four_way_handshake_is_about_the_password() {
    check(15, WifiProblem::Password, "password");
}

// 201, а с ним 210-212: сеть не нашлась - выключена, переименована, слишком
// слаба или переведена на несовместимую защиту (роутер на WPA3-only)
void test_no_ap_found_covers_the_whole_family() {
    check(201, WifiProblem::NotFound, "not_found");
    check(210, WifiProblem::NotFound, "not_found");
    check(211, WifiProblem::NotFound, "not_found");
    check(212, WifiProblem::NotFound, "not_found");
}

// Маяки пропали: питание выключили
void test_beacon_timeout_is_a_router_gone() {
    check(200, WifiProblem::Dropped, "dropped");
}

/*
Кадр от точки доступа значит «связь была и кончилась», и не больше.

Сказать по коду 2 «неверный пароль» - самая дорогая ошибка из возможных:
человек с верным паролем перенаберёт его и сохранит. Если пароль и правда
сменили, следующая попытка даст авторитетные 202/204.
*/
void test_deauth_frame_only_means_the_link_died() {
    for (const int reason : {1, 2, 3, 4, 5, 6, 7, 8, 16, 24})
        check(reason, WifiProblem::Dropped, "dropped");
}

// Код станции, по которому сказать нечего, так и называется
void test_unknown_station_code_is_other() {
    check(203, WifiProblem::Other, "other");
    check(205, WifiProblem::Other, "other");
    check(12345, WifiProblem::Other, "other");
}

// Фраза есть у каждой причины: пустая строка на странице выглядит поломкой
void test_every_problem_has_text() {
    const WifiProblem all[] = {WifiProblem::Password, WifiProblem::NotFound,
                               WifiProblem::Dropped, WifiProblem::Other};
    for (const WifiProblem p : all) {
        TEST_ASSERT_TRUE_MESSAGE(std::strlen(wifi_problem_text(p)) > 0, wifi_problem_key(p));
        TEST_ASSERT_TRUE(std::strlen(wifi_problem_key(p)) > 0);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_no_reason_is_no_problem);
    RUN_TEST(test_station_says_the_password_was_refused);
    RUN_TEST(test_four_way_handshake_is_about_the_password);
    RUN_TEST(test_no_ap_found_covers_the_whole_family);
    RUN_TEST(test_beacon_timeout_is_a_router_gone);
    RUN_TEST(test_deauth_frame_only_means_the_link_died);
    RUN_TEST(test_unknown_station_code_is_other);
    RUN_TEST(test_every_problem_has_text);
    return UNITY_END();
}
