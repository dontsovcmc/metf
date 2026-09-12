#ifndef _METF_NTP_PACKET_h
#define _METF_NTP_PACKET_h

/*
Сборка и разбор пакетов NTP (RFC 5905, режим client/server).

Файл намеренно без Arduino.h и без сети: правила проверяются хостовыми тестами
(test/test_ntp_packet). Сеть и часы живут в NtpServer.

Формат короткий: заголовок 48 байт, четыре метки времени по 8 байт. Метка -
секунды от 1900 года и дробная часть в долях 2^-32, обе big-endian.
*/

#include <stdint.h>
#include <string.h>

#define NTP_PACKET_SIZE 48
#define NTP_PORT 123

/*
Секунд между 1900 и 1970. NTP считает от 1900, unix - от 1970.
*/
#define NTP_UNIX_DELTA 2208988800UL

// Смещения меток времени в пакете
#define NTP_OFFSET_ORIGINATE 24
#define NTP_OFFSET_RECEIVE 32
#define NTP_OFFSET_TRANSMIT 40

#define NTP_MODE_CLIENT 3
#define NTP_MODE_SERVER 4

/*
Это запрос клиента?

Проверяем длину и режим. Индикатор високосной секунды у клиента почти всегда
3 ("часы не синхронизированы") - это нормально и отказом не является.
*/
inline bool ntp_is_client_request(const uint8_t *packet, const uint32_t size)
{
    if (packet == 0 || size < NTP_PACKET_SIZE)
        return false;

    return (packet[0] & 0x07) == NTP_MODE_CLIENT;
}

/*
Записать метку времени по смещению.
*/
inline void ntp_write_timestamp(uint8_t *packet, const uint32_t offset,
                                const uint32_t epoch, const uint32_t msec)
{
    const uint32_t secs = epoch + NTP_UNIX_DELTA;

    packet[offset + 0] = (uint8_t)(secs >> 24);
    packet[offset + 1] = (uint8_t)(secs >> 16);
    packet[offset + 2] = (uint8_t)(secs >> 8);
    packet[offset + 3] = (uint8_t)(secs);

    // Доля секунды в 2^-32: msec/1000 * 2^32. Считаем в 64 битах, иначе
    // умножение переполнится уже на 5 миллисекундах.
    const uint32_t fraction = (uint32_t)(((uint64_t)msec << 32) / 1000ULL);

    packet[offset + 4] = (uint8_t)(fraction >> 24);
    packet[offset + 5] = (uint8_t)(fraction >> 16);
    packet[offset + 6] = (uint8_t)(fraction >> 8);
    packet[offset + 7] = (uint8_t)(fraction);
}

/*
Прочитать секунды метки как unix-время. Ноль, если метка до 1970 года.
*/
inline uint32_t ntp_read_epoch(const uint8_t *packet, const uint32_t offset)
{
    uint32_t secs = (uint32_t)packet[offset + 0] << 24;
    secs |= (uint32_t)packet[offset + 1] << 16;
    secs |= (uint32_t)packet[offset + 2] << 8;
    secs |= (uint32_t)packet[offset + 3];

    return secs > NTP_UNIX_DELTA ? secs - NTP_UNIX_DELTA : 0;
}

/*
Собрать ответ сервера на запрос клиента.

@param out        буфер NTP_PACKET_SIZE байт
@param request    запрос клиента (нужен для версии и метки отправления)
@param recv_epoch unix-время получения запроса
@param recv_msec  миллисекунды того же момента
@param tx_epoch   unix-время отправки ответа
@param tx_msec    миллисекунды того же момента

Ватериус читает только метку отправления (`core/timekeeping.cpp`,
parse_ntp_packet), но заполняем все четыре: сервер, годный лишь для одного
клиента, проверить нечем, а разница в три строки.
*/
inline void ntp_build_reply(uint8_t *out, const uint8_t *request,
                            const uint32_t recv_epoch, const uint32_t recv_msec,
                            const uint32_t tx_epoch, const uint32_t tx_msec)
{
    memset(out, 0, NTP_PACKET_SIZE);

    // Версию возвращаем ту же, что прислал клиент: на неё смотрят строгие
    // реализации, а нам она ничего не стоит.
    const uint8_t version = (request[0] >> 3) & 0x07;

    out[0] = (uint8_t)((version << 3) | NTP_MODE_SERVER);  // индикатор 0: часы в порядке
    out[1] = 1;      // stratum 1 - первичный источник
    out[2] = request[2];   // интервал опроса - как просил клиент
    out[3] = 0xEC;   // точность, 2^-20 с

    // Задержка и разброс нулевые: источник тут же, в этой же плате
    memcpy(out + 12, "METF", 4);   // идентификатор источника

    // Опорная метка = момент получения: своих часов, кроме заданных по HTTP,
    // у платы нет, и притворяться, что синхронизация была раньше, незачем
    ntp_write_timestamp(out, 16, recv_epoch, recv_msec);
    memcpy(out + NTP_OFFSET_ORIGINATE, request + NTP_OFFSET_TRANSMIT, 8);
    ntp_write_timestamp(out, NTP_OFFSET_RECEIVE, recv_epoch, recv_msec);
    ntp_write_timestamp(out, NTP_OFFSET_TRANSMIT, tx_epoch, tx_msec);
}

#endif
