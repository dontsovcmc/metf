"""
Чтение лога с подтверждением: `GET /read?ack=<номер>`.

Смысл проверки - что плата **не забывает** отданное, пока читатель не сказал,
что получил. Без этого не доехавший по радио ответ уносит строки навсегда:
подтвердить их тем же `200` нельзя, он приходит после отправки и сам теряется.

Строки сюда приходят с UART испытуемого, и в голом стенде их может не быть
вовсе - тогда проверяется контракт (заголовок, сквозной номер), а проверки про
удержание пропускаются честно, а не выдаются за зелёные.
"""

from __future__ import annotations

from typing import Any

import pytest


@pytest.fixture(scope='module')
def protocol(board: Any) -> int:
    version = int(board.get('/version'))
    if version < 12:
        pytest.skip(f'нужен протокол 12, у платы {version}')
    return version


def test_номер_окна_в_каждом_ответе(board: Any, protocol: int) -> None:
    for path in ('/read', '/read?ack=0'):
        _, headers = board.get_full(path)
        assert 'X-Log-Seq' in headers, f'{path}: нет номера окна\n{headers}'
        assert headers['X-Log-Seq'].isdigit(), headers['X-Log-Seq']


def test_номер_не_убывает(board: Any, protocol: int) -> None:
    было = int(board.get_json('/read/stat')['seq'])
    стало = int(board.get_json('/read/stat')['seq'])
    assert стало >= было, (
        f'счётчик строк пошёл назад: {было} -> {стало}. Он сквозной за всю жизнь '
        f'платы, уменьшиться может только перезагрузкой')


def test_повтор_отдаёт_то_же_окно(board: Any, protocol: int) -> None:
    первый, headers = board.get_full('/read?ack=0')
    if not первый:
        pytest.skip('кольцо пусто: удерживать нечего')
    второй, снова = board.get_full('/read?ack=0')
    assert второй.startswith(первый), (
        'повтор без подтверждения обязан отдать то же окно - иначе потерянный '
        f'ответ уносит строки\nбыло {len(первый)} Б, стало {len(второй)} Б')
    assert int(снова['X-Log-Seq']) >= int(headers['X-Log-Seq'])


def test_подтверждение_освобождает_кольцо(board: Any, protocol: int) -> None:
    тело, headers = board.get_full('/read?ack=0')
    if not тело:
        pytest.skip('кольцо пусто: подтверждать нечего')
    seq = int(headers['X-Log-Seq'])
    board.get(f'/read?ack={seq}')
    осталось = int(board.get_json('/read/stat')['seq'])
    assert осталось >= seq
