"""
Плату находят по имени: `metf.local` (протокол 15).

Адрес выдаёт DHCP, и он меняется сам - при перезагрузке роутера, при
истечении аренды. Клиент, помнящий вчерашний адрес, не жалуется: он молчит по
таймауту, и искать беду приходится руками. Имя это переживает.
"""

from __future__ import annotations

import socket
import urllib.request
from typing import Any

import pytest

NAME = 'metf.local'
HTTP_TIMEOUT = 5.0


def resolve(name: str) -> str:
    try:
        return socket.gethostbyname(name)
    except socket.gaierror as err:
        pytest.fail(f'{name} не разрешается: {err}')


def test_имя_разрешается_в_адрес_платы(board: Any) -> None:
    assert resolve(NAME) == resolve(board.host)


def test_по_имени_отвечает_та_же_плата(board: Any) -> None:
    """Мало разрешить имя - по нему должна открываться сама плата."""
    with urllib.request.urlopen(f'http://{NAME}/version', timeout=HTTP_TIMEOUT) as answer:
        by_name = answer.read().decode().strip()

    assert by_name == board.get('/version').strip()


def test_протокол_не_старше_пятнадцатого(board: Any) -> None:
    """Имя появилось в пятнадцатом: на прошивке постарше искать нечего."""
    assert int(board.get('/version')) >= 15
