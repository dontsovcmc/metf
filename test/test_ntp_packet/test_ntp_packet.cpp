#include <unity.h>
#include <cstring>

#include "ntp_packet.h"

/*
Сборка пакета NTP: правила из RFC 5905 и то, что от ответа требует клиент.

Требования клиента взяты не из головы. Ватериус принимает ответ, только если
(`ESP8266/src/core/timekeeping.cpp`, parse_ntp_packet):

  - длина не меньше 48 байт;
  - старшие два бита нулевого байта не равны 11 (иначе «часы сервера не
    синхронизированы» и ответ отбрасывается);
  - секунды метки отправления больше 2208988800, то есть время не раньше 1970.

Ответ, нарушающий любое из трёх, выглядит как рабочий - и молча не работает.
Поэтому проверяются все три.
*/

// Типовой запрос Ватериуса: LI=3, версия 4, режим 3 (клиент)
static void make_request(uint8_t *req)
{
    memset(req, 0, NTP_PACKET_SIZE);
    req[0] = 0b11100011;
    req[1] = 0;
    req[2] = 6;
    req[3] = 0xEC;
    req[12] = 49;
    req[13] = 0x4E;
    req[14] = 49;
    req[15] = 52;
}

void test_request_recognized()
{
    uint8_t req[NTP_PACKET_SIZE];
    make_request(req);

    TEST_ASSERT_TRUE(ntp_is_client_request(req, NTP_PACKET_SIZE));
}

void test_short_packet_is_not_a_request()
{
    uint8_t req[NTP_PACKET_SIZE];
    make_request(req);

    TEST_ASSERT_FALSE(ntp_is_client_request(req, NTP_PACKET_SIZE - 1));
    TEST_ASSERT_FALSE(ntp_is_client_request(nullptr, NTP_PACKET_SIZE));
}

/*
Чужой ответ на наш порт - не запрос. Иначе плата отвечала бы сама себе и
своему же соседу по сети, а счётчик обращений врал бы.
*/
void test_server_packet_is_not_a_request()
{
    uint8_t reply[NTP_PACKET_SIZE];
    memset(reply, 0, NTP_PACKET_SIZE);
    reply[0] = (4 << 3) | NTP_MODE_SERVER;

    TEST_ASSERT_FALSE(ntp_is_client_request(reply, NTP_PACKET_SIZE));
}

void test_reply_is_acceptable_to_client()
{
    uint8_t req[NTP_PACKET_SIZE];
    uint8_t out[NTP_PACKET_SIZE];
    make_request(req);

    const uint32_t epoch = 1789000000UL;   // 2026 год
    ntp_build_reply(out, req, epoch, 250, epoch, 300);

    // Индикатор високосной секунды не 11: иначе клиент отбросит ответ
    TEST_ASSERT_NOT_EQUAL(0b11000000, out[0] & 0b11000000);
    // Режим - сервер
    TEST_ASSERT_EQUAL(NTP_MODE_SERVER, out[0] & 0x07);
    // Время отправления - после 1970 и равно назначенному
    TEST_ASSERT_EQUAL_UINT32(epoch, ntp_read_epoch(out, NTP_OFFSET_TRANSMIT));
}

/*
Версию возвращаем клиентскую: строгие реализации сверяют её, а нам это ничего
не стоит.
*/
void test_reply_keeps_client_version()
{
    uint8_t req[NTP_PACKET_SIZE];
    uint8_t out[NTP_PACKET_SIZE];

    for (uint8_t version = 1; version <= 4; version++)
    {
        make_request(req);
        req[0] = (uint8_t)(0b11000000 | (version << 3) | NTP_MODE_CLIENT);

        ntp_build_reply(out, req, 1789000000UL, 0, 1789000000UL, 0);

        TEST_ASSERT_EQUAL_UINT8(version, (out[0] >> 3) & 0x07);
    }
}

/*
Метка отправления клиента возвращается в поле originate - по ней клиент
опознаёт, что ответ на его запрос, а не на чей-то чужой.
*/
void test_reply_echoes_client_transmit_stamp()
{
    uint8_t req[NTP_PACKET_SIZE];
    uint8_t out[NTP_PACKET_SIZE];
    make_request(req);

    for (uint8_t i = 0; i < 8; i++)
        req[NTP_OFFSET_TRANSMIT + i] = (uint8_t)(0xA0 + i);

    ntp_build_reply(out, req, 1789000000UL, 0, 1789000000UL, 0);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(req + NTP_OFFSET_TRANSMIT,
                                  out + NTP_OFFSET_ORIGINATE, 8);
}

/*
Время получения не позже времени отправления: обратный порядок дал бы клиенту
отрицательную задержку, и компенсация уехала бы в минус.
*/
void test_receive_is_not_after_transmit()
{
    uint8_t req[NTP_PACKET_SIZE];
    uint8_t out[NTP_PACKET_SIZE];
    make_request(req);

    ntp_build_reply(out, req, 1789000000UL, 900, 1789000001UL, 100);

    TEST_ASSERT_TRUE(ntp_read_epoch(out, NTP_OFFSET_RECEIVE)
                     <= ntp_read_epoch(out, NTP_OFFSET_TRANSMIT));
}

/*
Дробная часть - доли 2^-32, а не миллисекунды. Ошибка здесь не видна ни в
одном тесте на секунды, а клиенту даёт полсекунды промаха.
*/
void test_fraction_encodes_milliseconds()
{
    uint8_t packet[NTP_PACKET_SIZE];
    memset(packet, 0, NTP_PACKET_SIZE);

    ntp_write_timestamp(packet, NTP_OFFSET_TRANSMIT, 1789000000UL, 500);

    uint32_t fraction = (uint32_t)packet[NTP_OFFSET_TRANSMIT + 4] << 24;
    fraction |= (uint32_t)packet[NTP_OFFSET_TRANSMIT + 5] << 16;
    fraction |= (uint32_t)packet[NTP_OFFSET_TRANSMIT + 6] << 8;
    fraction |= (uint32_t)packet[NTP_OFFSET_TRANSMIT + 7];

    // 500 мс - ровно половина от 2^32
    TEST_ASSERT_UINT32_WITHIN(1000, 0x80000000UL, fraction);
}

/*
Девятьсот миллисекунд - проверка на переполнение: наивное msec << 32 в 32
битах обнуляется, и дробная часть всегда выходила бы нулём.
*/
void test_fraction_does_not_overflow()
{
    uint8_t packet[NTP_PACKET_SIZE];
    memset(packet, 0, NTP_PACKET_SIZE);

    ntp_write_timestamp(packet, NTP_OFFSET_TRANSMIT, 1789000000UL, 900);

    uint32_t fraction = (uint32_t)packet[NTP_OFFSET_TRANSMIT + 4] << 24;
    fraction |= (uint32_t)packet[NTP_OFFSET_TRANSMIT + 5] << 16;

    TEST_ASSERT_TRUE(fraction > 0);
}

/*
Метка 1900 года читается нулём, а не отрицательным временем: у клиента это
вычитание 70 лет из беззнакового, то есть часы в 2484 году.
*/
void test_epoch_before_1970_reads_as_zero()
{
    uint8_t packet[NTP_PACKET_SIZE];
    memset(packet, 0, NTP_PACKET_SIZE);

    TEST_ASSERT_EQUAL_UINT32(0, ntp_read_epoch(packet, NTP_OFFSET_TRANSMIT));
}

int main(int argc, char **argv)
{
    UNITY_BEGIN();
    RUN_TEST(test_request_recognized);
    RUN_TEST(test_short_packet_is_not_a_request);
    RUN_TEST(test_server_packet_is_not_a_request);
    RUN_TEST(test_reply_is_acceptable_to_client);
    RUN_TEST(test_reply_keeps_client_version);
    RUN_TEST(test_reply_echoes_client_transmit_stamp);
    RUN_TEST(test_receive_is_not_after_transmit);
    RUN_TEST(test_fraction_encodes_milliseconds);
    RUN_TEST(test_fraction_does_not_overflow);
    RUN_TEST(test_epoch_before_1970_reads_as_zero);
    return UNITY_END();
}
