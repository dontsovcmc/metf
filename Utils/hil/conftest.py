"""
Стенд METF: живая плата, управляемый роутер и AT-плата в роли телефона.

Тесты идут по-настоящему: гасят роутер, уводят его на другой канал, меняют
на нём пароль, поднимают точку доступа платы и входят в неё с другой платы.
Поэтому они не запускаются случайно - нужен ключ --stand.

    pytest Utils/hil --stand -v

Адреса, порты и пароли - в stand.ini (см. stand.ini.example), сеть для
возврата платы - оттуда же или из secrets.ini.
"""

from __future__ import annotations

import configparser
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import router as router_mod
from atboard import AtBoard, AtError

ROOT = Path(__file__).resolve().parents[2]
HTTP_TIMEOUT = 5.0

OWN_AP_HOST = '192.168.4.1'   # адрес платы в её собственной точке
JOIN_S = 60.0                 # вход AT-платы в сеть: скан плюс ассоциация

# Молчание платы - это состояние стенда, а не поломка теста: ждущий
# цикл обязан его пережить, с какой бы стороны плата ни молчала
TRANSPORT_ERRORS = (OSError, ValueError, AtError)


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


# ------------------------------------------------------------------ плата


@dataclass
class Answer:
    status: int
    text: str
    headers: dict[str, str] = field(default_factory=dict)


class Client:
    """
    Плата METF по HTTP - независимо от того, как до неё добираются.

    Один и тот же разговор нужен с двух сторон: из сети стенда (рабочая
    машина) и изнутри точки доступа платы или роутера (AT-плата). Ответы
    платы и ожидания одинаковы, разный только транспорт - он и переопределяется
    в наследнике.
    """

    host = ''

    def get(self, path: str, timeout: float = HTTP_TIMEOUT) -> Answer:
        raise NotImplementedError

    def post(self, path: str, timeout: float = HTTP_TIMEOUT, **params: Any) -> Answer:
        raise NotImplementedError

    def wifi(self) -> dict[str, Any]:
        return json.loads(self.get('/wifi').text)

    def alive(self, timeout: float = 1.0) -> bool:
        try:
            return self.get('/ping', timeout=timeout).text.strip() == 'pong'
        except TRANSPORT_ERRORS:
            return False

    def wait_until(self, check: Callable[[dict], bool], timeout: float,
                   pause: float = 1.0, what: str = '') -> dict[str, Any]:
        """Ждать состояния платы. Возвращает последний ответ /wifi."""
        deadline = time.monotonic() + timeout
        last: dict[str, Any] = {}
        while time.monotonic() < deadline:
            try:
                last = self.wifi()
                if check(last):
                    return last
            except TRANSPORT_ERRORS:
                last = {}
            time.sleep(pause)
        raise AssertionError(f'плата не дождалась: {what or check}; последнее: {last}')


class Metf(Client):
    """Плата из сети стенда: обычный HTTP с рабочей машины."""

    def __init__(self, host: str) -> None:
        self.host = host

    def get(self, path: str, timeout: float = HTTP_TIMEOUT) -> Answer:
        return self._send(urllib.request.Request(f'http://{self.host}{path}'), timeout)

    def post(self, path: str, timeout: float = HTTP_TIMEOUT, **params: Any) -> Answer:
        body = urllib.parse.urlencode(params).encode()
        request = urllib.request.Request(f'http://{self.host}{path}', data=body, method='POST')
        return self._send(request, timeout)

    @staticmethod
    def _send(request: urllib.request.Request, timeout: float) -> Answer:
        """Отказ - такая же часть протокола, как успех: 4xx возвращаем, не бросаем."""
        try:
            with urllib.request.urlopen(request, timeout=timeout) as answer:
                return Answer(answer.status, answer.read().decode(), dict(answer.headers))
        except urllib.error.HTTPError as err:
            return Answer(err.code, err.read().decode(), dict(err.headers))


class Phone(Client):
    """
    Плата изнутри сети, куда рабочей машине не попасть: через AT-плату.

    Так тест видит METF, пока она сидит в собственной точке доступа или за
    NAT управляемого роутера.
    """

    def __init__(self, board: AtBoard, host: str) -> None:
        self.board = board
        self.host = host

    def get(self, path: str, timeout: float = 30.0) -> Answer:
        return _answer(self.board.get(path, self.host, timeout=timeout))

    def post(self, path: str, timeout: float = 30.0, **params: Any) -> Answer:
        body = urllib.parse.urlencode(params).encode()
        return _answer(self.board.post(path, self.host, body=body, timeout=timeout))


def _answer(response: Any) -> Answer:
    return Answer(response.status, response.text, response.headers)


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


# ------------------------------------------------------------------ роутер


@pytest.fixture(scope='session')
def router(stand: configparser.ConfigParser):
    """
    Управляемый роутер. Снимок настроек снимается до тестов и возвращается
    после: упавший тест иначе оставит стенд с погашенной точкой.
    """
    host = stand.get('router', 'host', fallback='')
    if not host:
        pytest.skip('нет [router] в stand.ini: беды роутера не проверить')
    try:
        device = router_mod.connect(host, stand.get('router', 'password', fallback=''),
                                    stand.get('router', 'ap_password', fallback=''))
        device.version()
    except (OSError, router_mod.RouterError) as err:
        pytest.skip(f'консоль роутера {host} не отвечает: {err}')

    before = device.snapshot()
    yield device
    try:
        device.restore(before)
    finally:
        device.close()


@pytest.fixture(scope='session')
def router_ap(router) -> tuple[str, str]:
    """Имя и пароль точки управляемого роутера: сеть, которую тесты ломают."""
    if not router.ap_password:
        pytest.skip('нужен [router] ap_password в stand.ini: пароль точки '
                    'у роутера не прочитать, show config печатает звёздочки')
    return router.ap_ssid(), router.ap_password
