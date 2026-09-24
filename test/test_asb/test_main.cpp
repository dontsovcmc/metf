#include <unity.h>

#include <algorithm>
#include <memory>

#include "AsyncSerialBuffer.h"

/*
Кольцо лога: чтение с подтверждением.

Главное здесь - не «строки печатаются», а то, что плата забывает строку только
после того, как читатель сказал, что получил её. Ответ по радио теряется, и
прежнее `/read` уносило строки навсегда: подтвердить их тем же `200` нельзя,
он приходит позже отправки и сам может не дойти.
*/

namespace {

std::unique_ptr<AsyncSerialBuffer> asb;

void push(const char *line) {
    for (const char *c = line; *c; ++c) asb->pushChar(*c);
    asb->pushChar('\n');
}

std::string read(uint32_t acked, uint32_t *seq = nullptr) {
    Print out;
    const uint32_t last = asb->read_to(out, acked);
    if (seq) *seq = last;
    return out.out;
}

} // namespace

void setUp() {
    asb = std::make_unique<AsyncSerialBuffer>();
}

void tearDown() {
    asb.reset();
}

// Читатель ничего не подтвердил - строки остаются на плате
void test_чтение_без_подтверждения_не_забывает(void) {
    push("раз");
    push("два");

    uint32_t seq = 0;
    TEST_ASSERT_EQUAL_STRING("раз\nдва\n", read(0, &seq).c_str());
    TEST_ASSERT_EQUAL_UINT32(2, seq);
    TEST_ASSERT_EQUAL_size_t(2, asb->count());
}

// Тот самый случай, ради которого всё затевалось: ответ не доехал, читатель
// повторил запрос и получил то же окно
void test_повтор_отдаёт_то_же_окно(void) {
    push("раз");
    push("два");

    const std::string first = read(0);
    const std::string again = read(0);
    TEST_ASSERT_EQUAL_STRING(first.c_str(), again.c_str());
}

// Подтверждение забывает ровно подтверждённое
void test_подтверждение_забывает_только_своё(void) {
    push("раз");
    push("два");
    push("три");

    TEST_ASSERT_EQUAL_STRING("два\nтри\n", read(1).c_str());
    TEST_ASSERT_EQUAL_size_t(2, asb->count());

    TEST_ASSERT_EQUAL_STRING("", read(3).c_str());
    TEST_ASSERT_EQUAL_size_t(0, asb->count());
}

// Строки, пришедшие между подтверждением и ответом, не теряются
void test_новые_строки_переживают_подтверждение(void) {
    push("раз");
    read(0);
    push("два");

    TEST_ASSERT_EQUAL_STRING("два\n", read(1).c_str());
}

// Номер растёт всю жизнь платы, а не с последнего чтения
void test_номер_сквозной(void) {
    push("раз");
    read(1);
    push("два");

    uint32_t seq = 0;
    read(1, &seq);
    TEST_ASSERT_EQUAL_UINT32(2, seq);
    TEST_ASSERT_EQUAL_UINT32(2, asb->seq());
}

// Подтверждение из будущего (плата перезагрузилась, читатель помнит старое)
// не должно выбрасывать лишнего сверх того, что есть
void test_подтверждение_больше_накопленного(void) {
    push("раз");
    TEST_ASSERT_EQUAL_STRING("", read(99).c_str());
    TEST_ASSERT_EQUAL_size_t(0, asb->count());
}

// Кольцо переполняется, пока читатель молчит: старейшее вытесняется и
// считается, а номера не разъезжаются
void test_переполнение_не_ломает_нумерацию(void) {
    for (size_t i = 0; i < ASB_MAX_LINES + 5; ++i) push("строка");

    TEST_ASSERT_TRUE(asb->dropped() > 0);
    uint32_t seq = 0;
    const std::string out = read(0, &seq);
    TEST_ASSERT_EQUAL_UINT32(ASB_MAX_LINES + 5, seq);
    TEST_ASSERT_EQUAL_UINT32(ASB_MAX_LINES + 5, asb->seq());
    // Осталось столько, сколько вмещает кольцо (одна ячейка всегда свободна)
    TEST_ASSERT_EQUAL_size_t(ASB_MAX_LINES - 1, asb->count());
    TEST_ASSERT_EQUAL_size_t(ASB_MAX_LINES - 1, std::count(out.begin(), out.end(), '\n'));
}

// Старый способ всё ещё работает: отдать и забыть
void test_устаревший_drain_забывает_сразу(void) {
    push("раз");
    Print out;
    asb->drain_to(out);
    TEST_ASSERT_EQUAL_STRING("раз\n", out.out.c_str());
    TEST_ASSERT_EQUAL_size_t(0, asb->count());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_чтение_без_подтверждения_не_забывает);
    RUN_TEST(test_повтор_отдаёт_то_же_окно);
    RUN_TEST(test_подтверждение_забывает_только_своё);
    RUN_TEST(test_новые_строки_переживают_подтверждение);
    RUN_TEST(test_номер_сквозной);
    RUN_TEST(test_подтверждение_больше_накопленного);
    RUN_TEST(test_переполнение_не_ломает_нумерацию);
    RUN_TEST(test_устаревший_drain_забывает_сразу);
    return UNITY_END();
}
