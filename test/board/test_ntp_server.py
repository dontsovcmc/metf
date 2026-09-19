"""
Сервер времени на живой плате.

Хостовые тесты (test/test_ntp_packet) проверяют сборку пакета, эти - что плата
действительно слушает 123, отвечает и отдаёт то время, которое ей назначили.
Одного без другого мало: пакет можно собрать верно и не отправить, а отправить
можно с неверного порта - и клиент молча не увидит ответа.

Запрос здесь собирается ровно такой, какой шлёт Ватериус
(`ESP8266/src/sync_time.cpp`, send_ntp_request), а ответ проверяется по тем же
правилам, по каким он его принимает (`core/timekeeping.cpp`, parse_ntp_packet).
Стенд тем самым проверяет не абстрактный NTP, а тот, которым будет пользоваться
устройство.

    pytest test/board --metf-host 192.168.1.50 -v
"""

from __future__ import annotations

import socket
import struct
import time
from typing import Any

import pytest

NTP_PORT = 123
NTP_PACKET_SIZE = 48
NTP_UNIX_DELTA = 2208988800

OFFSET_ORIGINATE = 24
OFFSET_TRANSMIT = 40

# Заведомо узнаваемое время: 2026-01-01 00:00:00 UTC. Отличается от настоящего
# на месяцы, поэтому «устройство взяло наше время» и «устройство взяло время
# откуда-то ещё» не перепутать.
TEST_EPOCH = 1767225600

REPLY_TIMEOUT = 2.0
SILENCE_TIMEOUT = 1.5     # столько ждём, чтобы утверждать «ответа нет»


def make_request() -> bytes:
    """Запрос клиента - байт в байт как у Ватериуса."""
    packet = bytearray(NTP_PACKET_SIZE)
    packet[0] = 0b11100011      # LI=3, версия 4, режим 3
    packet[1] = 0
    packet[2] = 6
    packet[3] = 0xEC
    packet[12] = 49
    packet[13] = 0x4E
    packet[14] = 49
    packet[15] = 52
    return bytes(packet)


def ask(host: str, request: bytes | None = None,
        timeout: float = REPLY_TIMEOUT) -> tuple[bytes, tuple[str, int]] | None:
    """Спросить время. Возвращает (ответ, адрес отправителя) или None."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        sock.sendto(request if request is not None else make_request(),
                    (host, NTP_PORT))
        return sock.recvfrom(512)
    except socket.timeout:
        return None
    finally:
        sock.close()


def transmit_epoch(reply: bytes) -> int:
    """unix-время из метки отправления."""
    secs = struct.unpack('!I', reply[OFFSET_TRANSMIT:OFFSET_TRANSMIT + 4])[0]
    return secs - NTP_UNIX_DELTA


@pytest.fixture
def serving(board: Any) -> Any:
    """Сервер поднят с известным временем; после теста остановлен."""
    board.post('/ntp', action='start', epoch=TEST_EPOCH)
    try:
        yield board
    finally:
        board.post('/ntp', action='drop', value=0)
        board.post('/ntp', action='stop')


def test_protocol_version(board: Any) -> None:
    """Сервер времени появился в шестой версии протокола."""
    assert int(board.get('/version')) >= 6, (
        'на плате прошивка старше: /ntp там нет')


def test_server_answers(serving: Any) -> None:
    """Плата отвечает на запрос и отдаёт назначенное время."""
    answer = ask(serving.host)
    assert answer is not None, 'плата не ответила на запрос NTP'
    reply, sender = answer

    assert len(reply) >= NTP_PACKET_SIZE, f'ответ короче 48 байт: {len(reply)}'
    # Клиент принимает ответ, только если он пришёл с порта NTP: ответ с
    # эфемерного порта для него всё равно что тишина
    assert sender[1] == NTP_PORT, f'ответ пришёл с порта {sender[1]}'
    # Индикатор «часы сервера не синхронизированы» - отказ для клиента
    assert reply[0] & 0b11000000 != 0b11000000, 'сервер объявил себя несинхронным'
    assert reply[0] & 0x07 == 4, 'режим в ответе не серверный'

    assert abs(transmit_epoch(reply) - TEST_EPOCH) <= 5, (
        f'плата отдала {transmit_epoch(reply)}, назначали {TEST_EPOCH}')


def test_reply_echoes_our_stamp(serving: Any) -> None:
    """
    В поле originate возвращается наша метка отправления.

    По ней клиент отличает ответ на свой запрос от чужого пакета, прилетевшего
    на тот же порт.
    """
    request = bytearray(make_request())
    request[OFFSET_TRANSMIT:OFFSET_TRANSMIT + 8] = bytes(range(0xB0, 0xB8))

    answer = ask(serving.host, bytes(request))
    assert answer is not None, 'плата не ответила'
    reply = answer[0]

    assert reply[OFFSET_ORIGINATE:OFFSET_ORIGINATE + 8] == bytes(range(0xB0, 0xB8)), (
        'метка клиента не вернулась в поле originate')


def test_clock_runs(serving: Any) -> None:
    """Часы идут: через две секунды плата отдаёт время на две секунды позже."""
    first = ask(serving.host)
    assert first is not None
    time.sleep(2.0)
    second = ask(serving.host)
    assert second is not None

    moved = transmit_epoch(second[0]) - transmit_epoch(first[0])
    assert 1 <= moved <= 4, f'за две секунды часы сдвинулись на {moved} с'


def test_time_can_be_reset(serving: Any) -> None:
    """action=time переставляет часы, не трогая слушателя."""
    moved_to = TEST_EPOCH + 86400 * 30
    serving.post('/ntp', action='time', epoch=moved_to)

    answer = ask(serving.host)
    assert answer is not None, 'после перевода часов плата замолчала'
    assert abs(transmit_epoch(answer[0]) - moved_to) <= 5


def test_stat_counts_requests(serving: Any) -> None:
    """Счётчики растут: по ним тесты стенда судят, дошёл ли запрос."""
    before = serving.get_json('/ntp/stat')
    assert before['running'] is True

    assert ask(serving.host) is not None
    after = serving.get_json('/ntp/stat')

    assert after['requests'] == before['requests'] + 1
    assert after['replies'] == before['replies'] + 1
    assert after['last_epoch'] >= TEST_EPOCH


def test_garbage_is_ignored(serving: Any) -> None:
    """
    Не запрос клиента - не ответ.

    Иначе плата отвечала бы на чужие пакеты, залетевшие на 123-й порт, и
    счётчик обращений врал бы тестам стенда.
    """
    before = serving.get_json('/ntp/stat')

    assert ask(serving.host, b'hello', timeout=SILENCE_TIMEOUT) is None, (
        'плата ответила на мусор'
    )
    # Ответ сервера, а не запрос клиента: режим 4 вместо 3
    server_shaped = bytearray(NTP_PACKET_SIZE)
    server_shaped[0] = (4 << 3) | 4
    assert ask(serving.host, bytes(server_shaped), timeout=SILENCE_TIMEOUT) is None

    after = serving.get_json('/ntp/stat')
    assert after['ignored'] >= before['ignored'] + 1
    assert after['replies'] == before['replies']


def test_drop_keeps_port_but_stays_silent(serving: Any) -> None:
    """
    Режим молчания: порт занят, ответа нет.

    Так выглядит недоступный сервер в интернете - клиент ждёт таймаут. От
    остановки отличается тем, что отказа порта клиент не получит.
    """
    serving.post('/ntp', action='drop', value=1)
    before = serving.get_json('/ntp/stat')
    assert before['dropping'] is True
    assert before['running'] is True

    assert ask(serving.host, timeout=SILENCE_TIMEOUT) is None, (
        'плата ответила, хотя её просили молчать')

    after = serving.get_json('/ntp/stat')
    assert after['dropped'] == before['dropped'] + 1
    assert after['replies'] == before['replies']

    # И снова отвечает, когда молчание снято
    serving.post('/ntp', action='drop', value=0)
    assert ask(serving.host) is not None


def test_stop_releases_the_port(serving: Any) -> None:
    """После остановки плата не отвечает и сообщает об этом в /ntp/stat."""
    serving.post('/ntp', action='stop')

    assert serving.get_json('/ntp/stat')['running'] is False
    assert ask(serving.host, timeout=SILENCE_TIMEOUT) is None, (
        'плата отвечает после остановки')


def test_bad_request_is_refused(board: Any) -> None:
    """Неизвестное действие и нулевое время отвергаются, а не применяются молча."""
    import urllib.error

    for params in ({'action': 'nonsense'},
                   {'action': 'start', 'epoch': 0},
                   {'action': 'start'}):
        with pytest.raises(urllib.error.HTTPError) as err:
            board.post('/ntp', **params)
        assert err.value.code == 400, f'{params}: код {err.value.code}'
