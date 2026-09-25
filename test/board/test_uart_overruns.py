"""
Счётчик переполнений приёмного буфера UART: `overruns` в `GET /read/stat`.

Зачем он вообще нужен. Кольцо лога считает только то, что вытеснило само
(`dropped`), а байты теряются и раньше кольца: UART разбирает `loop()`, и пока
он занят радио, драйвер складывает пришедшее в свой буфер. Переполнился - байты
пропали, до `pushChar` они не дошли, и кольцу их не в чем считать. Со стороны
это выглядит как дыра в логе при нулевом `dropped`, то есть как исправный лог.

Проверяется контракт: поле есть, оно целое и не убывает. Устроить переполнение
нарочно тест не может - для этого нужен посторонний источник данных на UART
испытуемого.
"""

from __future__ import annotations

from typing import Any

import pytest


@pytest.fixture(scope='module')
def protocol(board: Any) -> int:
    version = int(board.get('/version'))
    if version < 13:
        pytest.skip(f'нужен протокол 13, у платы {version}')
    return version


def test_счётчик_есть_и_он_целый(board: Any, protocol: int) -> None:
    stat = board.get_json('/read/stat')
    assert 'overruns' in stat, f'нет счётчика переполнений драйвера:\n{stat}'
    assert isinstance(stat['overruns'], int), stat['overruns']
    assert stat['overruns'] >= 0, stat['overruns']


def test_счётчик_не_убывает(board: Any, protocol: int) -> None:
    было = board.get_json('/read/stat')['overruns']
    стало = board.get_json('/read/stat')['overruns']
    assert стало >= было, (
        f'счётчик переполнений пошёл назад: {было} -> {стало}. Он сквозной за '
        f'всю жизнь платы, уменьшиться может только перезагрузкой')


def test_смена_скорости_не_гасит_счётчик(board: Any, protocol: int) -> None:
    """
    `HardwareSerial::end()` снимает обработчик ошибок, а смена скорости идёт
    через end()+begin(). Если ставить обработчик только в `begin()` платы,
    после первой же смены счётчик замолчит навсегда - и молчание будет
    неотличимо от «переполнений не было».
    """
    было = board.get_json('/read/stat')
    board.post('/serial', baudrate=57600)
    board.post('/serial', baudrate=было['baud'])

    стало = board.get_json('/read/stat')
    assert 'overruns' in стало, f'после смены скорости счётчик пропал:\n{стало}'
    assert стало['baud'] == было['baud'], стало
