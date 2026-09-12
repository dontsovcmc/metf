"""
Проверки живой платы: pytest, только стандартная библиотека.

Адрес платы обязателен и по умолчанию не задан - иначе тесты, запущенные без
стенда, пытались бы дозвониться неизвестно куда и падали бы по таймауту вместо
внятного пропуска.

    pytest test/board --metf-host 192.168.51.14 -v
"""

from __future__ import annotations

import json
import urllib.parse
import urllib.request
from typing import Any

import pytest

HTTP_TIMEOUT = 5.0


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption('--metf-host', default=None,
                     help='адрес платы METF, например 192.168.51.14')


@pytest.fixture(scope='session')
def host(request: pytest.FixtureRequest) -> str:
    value = request.config.getoption('--metf-host')
    if not value:
        pytest.skip('нужна плата: pytest --metf-host <ip>')
    return value


class Board:
    """Плата METF по HTTP."""

    def __init__(self, host: str) -> None:
        self.host = host

    def get(self, path: str) -> str:
        url = f'http://{self.host}{path}'
        with urllib.request.urlopen(url, timeout=HTTP_TIMEOUT) as answer:
            return answer.read().decode()

    def get_json(self, path: str) -> dict[str, Any]:
        return json.loads(self.get(path))

    def post(self, path: str, **params: Any) -> str:
        url = f'http://{self.host}{path}'
        body = urllib.parse.urlencode(params).encode()
        request = urllib.request.Request(url, data=body, method='POST')
        with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT) as answer:
            return answer.read().decode()


@pytest.fixture(scope='session')
def board(host: str) -> Board:
    return Board(host)
