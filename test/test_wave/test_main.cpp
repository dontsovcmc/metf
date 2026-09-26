#include <unity.h>

#include <initializer_list>

#include "wave.h"

/*
Правила пачки: сколько линий и участков, какой уровень на участке, сколько всё
это длится и что плата обязана отвергнуть.

Пачка нужна затем, что паузу между фронтами иначе отмеряет клиент через
Wi-Fi: заказанные 0,3 с на шаткой связи становились двумя секундами. Разбор -
в docs/api.md, раздел POST /pulse.
*/

namespace {

// Линия: вывод, уровень первого участка, участки
wave::Line line(const uint8_t pin, const uint8_t value,
                const std::initializer_list<uint32_t> edges) {
    wave::Line out{};
    out.pin = pin;
    out.value = value;
    out.at_ms = 0;
    out.count = 0;
    for (const uint32_t ms : edges) out.edges[out.count++] = ms;
    return out;
}

wave::Batch one(const wave::Line &l) {
    wave::Batch out{};
    out.count = 1;
    out.lines[0] = l;
    return out;
}

} // namespace

void setUp() {}
void tearDown() {}

void test_levels_alternate_from_the_asked_one() {
    // Импульс - это прижать и отпустить, поэтому уровень задаётся один раз
    const wave::Line low = line(2, 0, {300, 800, 300});

    TEST_ASSERT_EQUAL_UINT8(0, wave::level_at(low, 0));
    TEST_ASSERT_EQUAL_UINT8(1, wave::level_at(low, 1));
    TEST_ASSERT_EQUAL_UINT8(0, wave::level_at(low, 2));

    // Тип входа «Электронный (+)»: импульс - подъём линии, покой - земля
    const wave::Line high = line(3, 1, {1, 500});
    TEST_ASSERT_EQUAL_UINT8(1, wave::level_at(high, 0));
    TEST_ASSERT_EQUAL_UINT8(0, wave::level_at(high, 1));
}

void test_duration_is_the_whole_picture() {
    uint32_t total = 0;
    const wave::Batch batch = one(line(2, 0, {300, 800, 300}));

    TEST_ASSERT_EQUAL(wave::Error::None, wave::check(batch, total));
    TEST_ASSERT_EQUAL_UINT32(1400, total);
}

void test_offset_counts_into_duration() {
    // Смещением задаётся одновременность: импульс соседу внутрь замыкания
    uint32_t total = 0;
    wave::Batch batch{};
    batch.count = 2;
    batch.lines[0] = line(2, 0, {2000});
    batch.lines[1] = line(3, 0, {1});
    batch.lines[1].at_ms = 100;

    TEST_ASSERT_EQUAL(wave::Error::None, wave::check(batch, total));
    TEST_ASSERT_EQUAL_UINT32(2000, total);   // самая длинная линия

    batch.lines[1].at_ms = 2500;             // сосед позже конца первой линии
    TEST_ASSERT_EQUAL(wave::Error::None, wave::check(batch, total));
    TEST_ASSERT_EQUAL_UINT32(2501, total);
}

void test_empty_batch_is_refused() {
    uint32_t total = 0;
    wave::Batch batch{};
    batch.count = 0;

    TEST_ASSERT_EQUAL(wave::Error::NoLines, wave::check(batch, total));
}

void test_line_without_edges_is_refused() {
    uint32_t total = 0;
    wave::Batch batch = one(line(2, 0, {}));

    TEST_ASSERT_EQUAL(wave::Error::NoEdges, wave::check(batch, total));
}

void test_more_lines_than_slots_is_refused() {
    uint32_t total = 0;
    wave::Batch batch{};
    batch.count = wave::kMaxLines + 1;
    for (uint8_t i = 0; i < wave::kMaxLines; i++) batch.lines[i] = line(i, 0, {10});

    TEST_ASSERT_EQUAL(wave::Error::TooManyLines, wave::check(batch, total));
}

void test_more_edges_than_room_is_refused() {
    uint32_t total = 0;
    wave::Batch batch = one(line(2, 0, {10}));
    batch.lines[0].count = wave::kMaxEdges + 1;

    TEST_ASSERT_EQUAL(wave::Error::TooManyEdges, wave::check(batch, total));
}

void test_zero_length_edge_is_refused() {
    // Ноль миллисекунд - это не фронт, а заказ, которого плата не отмерит
    uint32_t total = 0;
    const wave::Batch batch = one(line(2, 0, {500, 0, 500}));

    TEST_ASSERT_EQUAL(wave::Error::ShortEdge, wave::check(batch, total));
}

void test_too_long_batch_is_refused() {
    uint32_t total = 0;
    const wave::Batch batch = one(line(2, 0, {wave::kMaxBatchMs, 1}));

    TEST_ASSERT_EQUAL(wave::Error::TooLong, wave::check(batch, total));
}

void test_batch_at_the_ceiling_is_allowed() {
    uint32_t total = 0;
    const wave::Batch batch = one(line(2, 0, {wave::kMaxBatchMs}));

    TEST_ASSERT_EQUAL(wave::Error::None, wave::check(batch, total));
    TEST_ASSERT_EQUAL_UINT32(wave::kMaxBatchMs, total);
}

void test_same_pin_twice_is_refused() {
    // Два таймера в один регистр - это гонка, а не заказанная форма
    uint32_t total = 0;
    wave::Batch batch{};
    batch.count = 2;
    batch.lines[0] = line(2, 0, {500});
    batch.lines[1] = line(2, 0, {500});
    batch.lines[1].at_ms = 800;

    TEST_ASSERT_EQUAL(wave::Error::DuplicatePin, wave::check(batch, total));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_levels_alternate_from_the_asked_one);
    RUN_TEST(test_duration_is_the_whole_picture);
    RUN_TEST(test_offset_counts_into_duration);
    RUN_TEST(test_empty_batch_is_refused);
    RUN_TEST(test_line_without_edges_is_refused);
    RUN_TEST(test_more_lines_than_slots_is_refused);
    RUN_TEST(test_more_edges_than_room_is_refused);
    RUN_TEST(test_zero_length_edge_is_refused);
    RUN_TEST(test_too_long_batch_is_refused);
    RUN_TEST(test_batch_at_the_ceiling_is_allowed);
    RUN_TEST(test_same_pin_twice_is_refused);
    return UNITY_END();
}
