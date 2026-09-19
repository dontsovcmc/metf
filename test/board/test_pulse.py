"""
Импульс на живой плате: он отмеряется точно и не останавливает сервер.

Выдержку когда-то делал сам обработчик - `delay(ms)` внутри callback'а
ESPAsyncWebServer. Callback работает в задаче async_tcp, общей для всех
соединений платы, поэтому четырёхсекундное нажатие кнопки на это время гасило
весь HTTP: стенд в это же время опрашивает `/read` каждые 100 мс и получал
таймауты, а к концу часового прогона плата переставала отвечать совсем.

Дальше выдержку отдали таймеру ядра, а ответ - отложили до её конца. Ответ
оказался плохими часами: его отдаёт не момент готовности, а ближайший опрос
AsyncTCP, и он опаздывал на 240-336 мс. Стенд отсчитывал от ответа паузу между
импульсами, и проверка слипания на attiny поехала.

Теперь плата отвечает сразу, а конец импульса клиент считает сам. Здесь
проверяется, что линия при этом действительно занята ровно столько, сколько
заказано, - мерит это сама плата отказом `409`.

    pytest test/board --metf-host 192.168.1.50 -v
"""

from __future__ import annotations

import time
import urllib.error
from typing import Any

import pytest

# Линия, не заведённая на устройство: пины 0..3 стенд держит под кнопку, сброс
# и входы счётчиков, и импульс по ним - это воздействие на Ватериус
FREE_PIN = 5
OTHER_FREE_PIN = 6

PULSE_MS = 2000
POLL_PAUSE_S = 0.1

# Ответ должен приходить сразу, а не в конце импульса. Порог - пятая часть
# выдержки: сетевой обмен укладывается в десятки миллисекунд, а прежнее
# поведение (ответ в конце) дало бы все 2 с
ANSWER_S = PULSE_MS / 1000.0 / 5

# Запас на дорогу и на разрешение таймера: столько ждём после конца импульса,
# прежде чем требовать, чтобы линия освободилась
RELEASE_MARGIN_S = 0.5

# Пока идёт импульс, посторонний запрос обязан отвечать как обычно. Порог
# взят с большим запасом: до правки такие запросы не отвечали вовсе
OTHER_REQUEST_S = 1.0


def test_протокол_не_старше_восьмого(board: Any) -> None:
    """До восьмой версии ответ приходил в конце импульса - проверки ниже про другое."""
    assert int(board.get('/version')) >= 8, (
        'на плате прошивка старше: /pulse там отвечает концом импульса')


def test_импульс_не_блокирует_остальные_запросы(board: Any) -> None:
    """Главное свойство: сервер жив всё время импульса."""
    answers: list[float] = []
    board.post('/pulse', pin=FREE_PIN, value=0, duration_ms=PULSE_MS)

    finish = time.monotonic() + PULSE_MS / 1000.0
    while time.monotonic() < finish:
        started = time.monotonic()
        board.get('/ping')
        answers.append(time.monotonic() - started)
        time.sleep(POLL_PAUSE_S)

    assert len(answers) >= 5, f'за импульс прошло всего {len(answers)} запросов'
    assert max(answers) < OTHER_REQUEST_S, (
        f'посторонний запрос ждал {max(answers):.2f} с - сервер был занят импульсом')


def test_ответ_приходит_сразу_и_называет_выдержку(board: Any) -> None:
    """
    Ответ - расписка о приёме, а не отметка о конце.

    Клиент по ней знает, сколько ждать, и ждёт по своим часам: плата про конец
    импульса больше не пишет, потому что вовремя написать не может.
    """
    started = time.monotonic()
    answer = board.post('/pulse', pin=FREE_PIN, value=0, duration_ms=PULSE_MS)
    spent = time.monotonic() - started

    assert answer.strip() == str(PULSE_MS), f'плата не назвала выдержку: {answer!r}'
    assert spent < ANSWER_S, f'ответ ждал конца импульса: {spent:.2f} с'

    time.sleep(PULSE_MS / 1000.0)        # освобождаем линию перед следующим тестом


def test_линия_занята_ровно_заказанное_время(board: Any) -> None:
    """
    Раз конец импульса теперь считает клиент, плата обязана держать выдержку
    честно: соврав, она даст не тот импульс, а узнать об этом будет неоткуда.

    Спрашиваем саму плату: пока импульс идёт, второй отвергается с `409`.
    """
    started = time.monotonic()
    board.post('/pulse', pin=FREE_PIN, value=0, duration_ms=PULSE_MS)

    time.sleep(PULSE_MS / 1000.0 / 2)    # середина импульса
    with pytest.raises(urllib.error.HTTPError) as err:
        board.post('/pulse', pin=FREE_PIN, value=0, duration_ms=100)
    assert err.value.code == 409, 'линия отпущена раньше времени'

    while time.monotonic() - started < PULSE_MS / 1000.0 + RELEASE_MARGIN_S:
        time.sleep(0.05)
    board.post('/pulse', pin=FREE_PIN, value=0, duration_ms=100)   # 409 здесь - падение
    time.sleep(0.2)


def test_импульсы_по_разным_выводам_идут_разом(board: Any) -> None:
    """
    Занят бывает вывод, а не плата.

    Стенд нажимает кнопку, пока по входу счётчика идёт серия импульсов, -
    один флаг на всю плату отвергал такое нажатие с `409`, и проверка
    «импульсы посреди сеанса не теряются» падала на ровном месте.
    """
    board.post('/pulse', pin=FREE_PIN, value=0, duration_ms=PULSE_MS)
    try:
        answer = board.post('/pulse', pin=OTHER_FREE_PIN, value=0,
                            duration_ms=PULSE_MS)
        assert answer.strip() == str(PULSE_MS), 'соседний вывод отвергнут'
    finally:
        time.sleep(PULSE_MS / 1000.0)


def test_второй_импульс_по_тому_же_выводу_отвергается(board: Any) -> None:
    """
    Линия одна, и наложить на неё второй импульс нельзя: заказчик должен
    увидеть отказ, а не молча получить импульс не той длины.
    """
    board.post('/pulse', pin=FREE_PIN, value=0, duration_ms=PULSE_MS)
    try:
        with pytest.raises(urllib.error.HTTPError) as err:
            board.post('/pulse', pin=FREE_PIN, value=0, duration_ms=200)
        assert err.value.code == 409
    finally:
        time.sleep(PULSE_MS / 1000.0)
