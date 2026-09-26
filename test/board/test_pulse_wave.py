"""
Форма сигнала на живой плате: интервалы между фронтами отмеряет она, а не клиент.

Пока плата умела только один уровень на один запрос, паузу между импульсами
набирал клиент через Wi-Fi: заказанные 0,3 с на шаткой связи превращались в две
секунды, и тест испытуемого винил в этом его прошивку. Теперь пачка фронтов
уезжает одним запросом, а плата отчитывается, что сделала, - по своим часам.

Проверяется именно отчёт: интервалы в нём обязаны совпасть с заказом. Если
плата соврёт, стенд об этом узнать не сможет - это последняя инстанция.

    pytest test/board --metf-host 192.168.1.50 -v
"""

from __future__ import annotations

import time
from typing import Any

import pytest

# Линии, не заведённые на устройство: пины 0..3 стенд держит под кнопку, сброс
# и входы счётчиков, и импульс по ним - это воздействие на Ватериус
FREE_PIN = 5
OTHER_FREE_PIN = 6

# Настоящее воздействие на счётчик: замкнуто 300 мс, отпущено 800 мс, замкнуто
# 300 мс. Пауза 800 мс - больше 750 мс, за которые attiny считает импульс
# кончившимся, поэтому это два импульса, а не один
SHAPE = [300, 800, 300]
SHAPE_MS = sum(SHAPE)

# Насколько моменты фронтов на плате могут разойтись с заказом. Таймер ядра
# отмеряет микросекундами, но колбэк ждёт своей очереди в задаче esp_timer
TOLERANCE_MS = 25

# Ответ - расписка о приёме, а не отметка о конце: порог с запасом на дорогу
ANSWER_S = 0.5

RELEASE_MARGIN_S = 0.3


def intervals(edges: list[int]) -> list[int]:
    """Длительности участков из моментов фронтов: их на один больше."""
    return [second - first for first, second in zip(edges, edges[1:])]


def line_of(stat: dict[str, Any], pin: int) -> dict[str, Any]:
    for line in stat['lines']:
        if line['pin'] == pin:
            return line
    raise AssertionError(f'в расписке нет вывода {pin}: {stat}')


@pytest.fixture
def idle(board: Any) -> None:
    """Линии свободны: чужая недоигранная пачка отвергла бы запрос кодом 409."""
    while board.get_json('/pulse/stat')['busy']:
        time.sleep(0.1)


def test_протокол_не_старше_четырнадцатого(board: Any) -> None:
    """До четырнадцатой версии формы сигнала нет, и проверки ниже бессмысленны."""
    assert int(board.get('/version')) >= 14, (
        'на плате прошивка старше: она не умеет пачку фронтов')


def test_пачка_принимается_сразу_и_называет_свою_длину(board: Any, idle: None) -> None:
    started = time.monotonic()
    code, answer = board.post_json('/pulse', {
        'lines': [{'pin': FREE_PIN, 'value': 0, 'edges': SHAPE}]})
    spent = time.monotonic() - started

    assert code == 202, answer
    assert answer.strip() == str(SHAPE_MS), f'плата не назвала длину пачки: {answer!r}'
    assert spent < ANSWER_S, f'ответ ждал конца пачки: {spent:.2f} с'

    time.sleep(SHAPE_MS / 1000.0 + RELEASE_MARGIN_S)


def test_интервалы_в_расписке_совпадают_с_заказом(board: Any, idle: None) -> None:
    """
    Главная проверка: 300 - 800 - 300 обязаны быть именно такими.

    Стенд по этой расписке утверждает, что подал; заказанному интервалу верить
    нельзя, его искажает дорога запроса.
    """
    code, answer = board.post_json('/pulse', {
        'lines': [{'pin': FREE_PIN, 'value': 0, 'edges': SHAPE}]})
    assert code == 202, answer

    time.sleep(SHAPE_MS / 1000.0 + RELEASE_MARGIN_S)

    line = line_of(board.get_json('/pulse/stat'), FREE_PIN)
    assert line['asked'] == SHAPE, line
    assert len(line['edges_ms']) == len(SHAPE) + 1, (
        f'моментов фронтов {len(line["edges_ms"])}, участков {len(SHAPE)}: '
        'последний момент - отпускание линии')

    got = intervals(line['edges_ms'])
    for i, (asked, real) in enumerate(zip(SHAPE, got)):
        assert abs(real - asked) <= TOLERANCE_MS, (
            f'участок {i}: заказано {asked} мс, плата отмерила {real} мс '
            f'(моменты {line["edges_ms"]})')


def test_смещение_кладёт_импульс_внутрь_замыкания(board: Any, idle: None) -> None:
    """
    Одновременность - это заказ, а не гонка запросов.

    Два входа разного типа надо проверять под импульсами одновременно: пока
    механический замкнут, электронный получает импульс в миллисекунду.
    """
    hold_ms = 2000
    at_ms = 100
    code, answer = board.post_json('/pulse', {'lines': [
        {'pin': FREE_PIN, 'value': 0, 'edges': [hold_ms]},
        {'pin': OTHER_FREE_PIN, 'value': 0, 'at_ms': at_ms, 'edges': [1]},
    ]})
    assert code == 202, answer
    assert answer.strip() == str(hold_ms), answer

    time.sleep(hold_ms / 1000.0 + RELEASE_MARGIN_S)

    stat = board.get_json('/pulse/stat')
    hold = line_of(stat, FREE_PIN)
    short = line_of(stat, OTHER_FREE_PIN)

    offset = short['edges_ms'][0] - hold['edges_ms'][0]
    assert abs(offset - at_ms) <= TOLERANCE_MS, (
        f'импульс ушёл через {offset} мс после замыкания, заказано {at_ms}')
    assert short['edges_ms'][-1] < hold['edges_ms'][-1], (
        'импульс соседа кончился позже замыкания - перекрытия не было')


def test_старая_форма_работает_как_прежде(board: Any, idle: None) -> None:
    """Кнопка, сброс и датчик протечки идут формой: один уровень на выдержку."""
    answer = board.post('/pulse', pin=FREE_PIN, value=0, duration_ms=300)
    assert answer.strip() == '300', answer

    time.sleep(0.3 + RELEASE_MARGIN_S)

    line = line_of(board.get_json('/pulse/stat'), FREE_PIN)
    assert line['asked'] == [300], line
    assert len(line['edges_ms']) == 2, line


def test_занятый_вывод_отвергает_пачку(board: Any, idle: None) -> None:
    """Отказ - про вывод: наложить второй заказ на ту же линию нельзя."""
    code, _ = board.post_json('/pulse', {
        'lines': [{'pin': FREE_PIN, 'value': 0, 'edges': [2000]}]})
    assert code == 202

    try:
        code, body = board.post_json('/pulse', {
            'lines': [{'pin': FREE_PIN, 'value': 0, 'edges': [100]}]})
        assert code == 409, f'{code}: {body}'
    finally:
        time.sleep(2.0 + RELEASE_MARGIN_S)


@pytest.mark.parametrize('body, why', [
    ({}, 'нет списка линий'),
    ({'lines': []}, 'пустая пачка'),
    ({'lines': [{'pin': FREE_PIN, 'edges': []}]}, 'линия без участков'),
    ({'lines': [{'pin': FREE_PIN, 'edges': [500, 0, 500]}]}, 'нулевой участок'),
    ({'lines': [{'pin': FREE_PIN, 'edges': [31000]}]}, 'пачка длиннее 30 с'),
    ({'lines': [{'edges': [100]}]}, 'линия без вывода'),
    ({'lines': [{'pin': FREE_PIN, 'edges': ['сто']}]}, 'участок не число'),
    ({'lines': [{'pin': FREE_PIN, 'value': 'да', 'edges': [100]}]}, 'уровень не число'),
    ({'lines': [{'pin': FREE_PIN, 'edges': [100]},
                {'pin': FREE_PIN, 'at_ms': 500, 'edges': [100]}]}, 'вывод дважды'),
])
def test_негодная_пачка_отвергается_словами(board: Any, idle: None,
                                            body: dict[str, Any], why: str) -> None:
    """
    Отказ обязан называть причину: тест стенда, получив её, расскажет человеку,
    что не так с воздействием, а не про прошивку испытуемого.
    """
    code, answer = board.post_json('/pulse', body)

    assert code == 400, f'{why}: плата ответила {code} - {answer}'
    assert answer.strip(), f'{why}: отказ без объяснения'
