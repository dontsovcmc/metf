"""
Импульс на живой плате: он отмеряется точно и не останавливает сервер.

Выдержку раньше делал сам обработчик - `delay(ms)` внутри callback'а
ESPAsyncWebServer. Callback работает в задаче async_tcp, общей для всех
соединений платы, поэтому четырёхсекундное нажатие кнопки на это время гасило
весь HTTP: стенд в это же время опрашивает `/read` каждые 100 мс и получал
таймауты, а к концу часового прогона плата переставала отвечать совсем.

    pytest test/board --metf-host 192.168.1.50 -v
"""

from __future__ import annotations

import threading
import time
import urllib.error
from typing import Any

import pytest

# Линия, не заведённая на устройство: пины 0..3 стенд держит под кнопку, сброс
# и входы счётчиков, и импульс по ним - это воздействие на Ватериус
FREE_PIN = 5

PULSE_MS = 2000
POLL_PAUSE_S = 0.1

# Ответ уходит на ближайшем poll-событии соединения, а lwip зовёт его раз в
# полсекунды (CONFIG_ASYNC_TCP_POLL_TIMER=1), поэтому запас - секунда с лишним
ANSWER_LAG_S = 1.5

# Пока идёт импульс, посторонний запрос обязан отвечать как обычно. Порог
# взят с большим запасом: до правки такие запросы не отвечали вовсе
OTHER_REQUEST_S = 1.0


def test_импульс_не_блокирует_остальные_запросы(board: Any) -> None:
    """Главное свойство: сервер жив всё время импульса."""
    answers: list[float] = []

    def ping_until(stop: threading.Event) -> None:
        while not stop.is_set():
            started = time.monotonic()
            board.get('/ping')
            answers.append(time.monotonic() - started)
            time.sleep(POLL_PAUSE_S)

    stop = threading.Event()
    pinger = threading.Thread(target=ping_until, args=(stop,), daemon=True)
    pinger.start()
    try:
        board.post('/pulse', pin=FREE_PIN, value=0, duration_ms=PULSE_MS)
    finally:
        stop.set()
        pinger.join(timeout=5)

    assert len(answers) >= 5, f'за импульс прошло всего {len(answers)} запросов'
    assert max(answers) < OTHER_REQUEST_S, (
        f'посторонний запрос ждал {max(answers):.2f} с - сервер был занят импульсом')


def test_ответ_приходит_после_импульса(board: Any) -> None:
    """Контракт клиента прежний: ответ означает, что линия уже отпущена."""
    started = time.monotonic()
    answer = board.post('/pulse', pin=FREE_PIN, value=0, duration_ms=PULSE_MS)
    spent = time.monotonic() - started

    assert answer.strip() == 'OK'
    assert spent >= PULSE_MS / 1000.0, f'ответ пришёл раньше конца импульса: {spent:.2f} с'
    assert spent < PULSE_MS / 1000.0 + ANSWER_LAG_S, f'ответ опоздал: {spent:.2f} с'


def test_второй_импульс_во_время_первого_отвергается(board: Any) -> None:
    """
    Линия одна, и наложить на неё второй импульс нельзя: заказчик должен
    увидеть отказ, а не молча получить импульс не той длины.
    """
    first = threading.Thread(
        target=board.post, args=('/pulse',),
        kwargs={'pin': FREE_PIN, 'value': 0, 'duration_ms': PULSE_MS}, daemon=True)
    first.start()
    time.sleep(0.3)                      # даём первому начаться

    try:
        with pytest.raises(urllib.error.HTTPError) as err:
            board.post('/pulse', pin=FREE_PIN, value=0, duration_ms=200)
        assert err.value.code == 409
    finally:
        first.join(timeout=PULSE_MS / 1000.0 + 5)
