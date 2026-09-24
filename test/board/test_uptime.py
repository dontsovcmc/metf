"""
Заголовок `X-Uptime-Ms` — мс с момента старта платы, в каждом ответе.

По нему стенд узнаёт, что плата перезагрузилась: число уменьшилось - значит был
старт заново. Отличить перезагрузку от занятости больше нечем, а последствия у
неё заметные: кольцо лога пусто, сервер времени выключен, выводы вернулись во
вход. Без признака в этом винят испытуемого.
"""

from __future__ import annotations

import time

import pytest

UPTIME = 'X-Uptime-Ms'

# Ручки разных слоёв: стенд (BenchRoutes), сеть (WifiPortal) и отказ, который
# собирается третьим путём (http::send_400 и onNotFound)
ROUTES = ['/ping', '/version', '/read/stat', '/wifi', '/no-such-route']


def test_протокол_не_старше_одиннадцатого(board) -> None:
    assert int(board.get('/version')) >= 11


@pytest.mark.parametrize('path', ROUTES)
def test_заголовок_есть_в_каждом_ответе(board, path: str) -> None:
    headers = board.headers(path)
    assert UPTIME in headers, f'{path}: нет заголовка {UPTIME}, есть {list(headers)}'
    assert headers[UPTIME].isdigit(), f'{path}: {UPTIME}={headers[UPTIME]!r} - не число'


def test_аптайм_растёт_не_медленнее_часов(board) -> None:
    """
    Между двумя запросами плата не перезагружалась, значит число выросло - и
    выросло не меньше, чем прошло времени у нас: иначе это не аптайм.
    """
    before = int(board.headers('/ping')[UPTIME])
    started = time.monotonic()
    time.sleep(2.0)
    after = int(board.headers('/ping')[UPTIME])
    passed = (time.monotonic() - started) * 1000

    assert after > before, f'аптайм не вырос: {before} -> {after}'
    assert after - before >= passed * 0.9, (
        f'аптайм вырос на {after - before} мс, а времени прошло {passed:.0f} мс')
