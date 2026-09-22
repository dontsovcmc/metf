"""
Стенд METF: живая плата плюс AT-плата в роли телефона.

Тесты идут по-настоящему: поднимают точку доступа, входят в неё с другой
платы, открывают страницу настройки и меняют через неё сеть. Поэтому они не
запускаются случайно - нужен ключ --stand.

    pytest Utils/hil --stand -v

Адреса и порты - в stand.ini (см. stand.ini.example), сеть для возврата платы -
оттуда же или из secrets.ini.
"""

from __future__ import annotations

import configparser
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from atboard import AtBoard  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
HTTP_TIMEOUT = 5.0


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption('--stand', action='store_true',
                     help='гонять на живом стенде: плата и AT-плата подключены')


def pytest_configure(config: pytest.Config) -> None:
    config.addinivalue_line('markers', 'slow: минуты, а не секунды')


def pytest_collection_modifyitems(config: pytest.Config, items: list[pytest.Item]) -> None:
    if config.getoption('--stand'):
        return
    skip = pytest.mark.skip(reason='нужен стенд: pytest Utils/hil --stand')
    for item in items:
        item.add_marker(skip)


@pytest.fixture(scope='session')
def stand() -> configparser.ConfigParser:
    path = Path(__file__).parent / 'stand.ini'
    if not path.exists():
        pytest.skip('нет Utils/hil/stand.ini, см. stand.ini.example')
    cfg = configparser.ConfigParser()
    cfg.read(path)
    return cfg


@pytest.fixture(scope='session')
def network(stand: configparser.ConfigParser) -> tuple[str, str]:
    """Сеть, в которую тест возвращает плату: из stand.ini, иначе из secrets.ini."""
    ssid = stand.get('network', 'ssid', fallback='')
    password = stand.get('network', 'password', fallback='')
    if ssid:
        return ssid, password

    secrets = ROOT / 'secrets.ini'
    if not secrets.exists():
        pytest.skip('сеть не задана: ни в stand.ini, ни в secrets.ini')
    cfg = configparser.ConfigParser()
    cfg.read(secrets)
    ssid = cfg.get('secrets', 'wifi_ssid', fallback='')
    if not ssid:
        pytest.skip('в secrets.ini нет wifi_ssid')
    return ssid, cfg.get('secrets', 'wifi_password', fallback='')


class Metf:
    """Плата по HTTP из сети стенда."""

    def __init__(self, host: str) -> None:
        self.host = host

    def get(self, path: str, timeout: float = HTTP_TIMEOUT) -> str:
        with urllib.request.urlopen(f'http://{self.host}{path}', timeout=timeout) as answer:
            return answer.read().decode()

    def wifi(self) -> dict[str, Any]:
        return json.loads(self.get('/wifi'))

    def post(self, path: str, **params: Any) -> tuple[int, str]:
        body = urllib.parse.urlencode(params).encode()
        request = urllib.request.Request(f'http://{self.host}{path}', data=body, method='POST')
        try:
            with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT) as answer:
                return answer.status, answer.read().decode()
        except urllib.error.HTTPError as err:
            return err.code, err.read().decode()

    def alive(self, timeout: float = 1.0) -> bool:
        try:
            return self.get('/ping', timeout=timeout).strip() == 'pong'
        except OSError:
            return False

    def wait_until(self, check, timeout: float, pause: float = 1.0, what: str = '') -> dict:
        """Ждать состояния платы. Возвращает последний ответ /wifi."""
        deadline = time.monotonic() + timeout
        last: dict[str, Any] = {}
        while time.monotonic() < deadline:
            try:
                last = self.wifi()
                if check(last):
                    return last
            except OSError:
                last = {}
            time.sleep(pause)
        raise AssertionError(f'плата не дождалась: {what or check}; последнее: {last}')


@pytest.fixture(scope='session')
def metf(stand: configparser.ConfigParser) -> Metf:
    board = Metf(stand.get('metf', 'host'))
    if not board.alive(timeout=3):
        pytest.skip(f'плата не отвечает на {board.host}')
    return board


@pytest.fixture(scope='session')
def atboard(stand: configparser.ConfigParser) -> AtBoard:
    """
    AT-плата на всю сессию: открытие порта - это её перезагрузка, а она
    занимает секунды.
    """
    port = stand.get('atboard', 'port')
    try:
        board = AtBoard(port)
    except OSError as err:
        pytest.skip(f'AT-плата не открылась на {port}: {err}')
    yield board
    board.close()   # гасит радио: забытая станция греется и ищет точку вечно
