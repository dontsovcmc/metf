"""
Портал METF на живом железе: точка доступа, страница настройки, смена сети.

Это единственная часть прошивки, которую нельзя проверить с рабочей машины:
её радио занято сетью стенда, а уйти в точку платы - значит потерять и стенд,
и сеть. Поэтому в точку входит AT-плата, а рабочая машина смотрит со стороны
роутера. Ради этого стенд и собран - см. README.md.

Проверяется то, что делает человек с телефоном: увидеть сеть METF-XXXX, войти
в неё, открыть страницу (её открывает сам телефон - это captive-редирект),
выбрать сеть, ввести пароль и увидеть новый адрес платы.

    pytest Utils/hil --stand -v
"""

from __future__ import annotations

import contextlib
import json
import time

import pytest
from conftest import JOIN_S, OWN_AP_HOST, Phone

from atboard import AtBoard, AtError

HOST = OWN_AP_HOST        # адрес платы в её собственной точке

AP_WAIT_S = 20.0          # от команды до поднятой точки
SCAN_WAIT_S = 45.0        # сколько сканируем эфир, прежде чем сдаться
RETURN_WAIT_S = 90.0      # от смены сети до появления платы в сети стенда


@pytest.fixture(scope='module')
def ap(metf):
    """
    Поднятая точка доступа на всё время модуля.

    Гасим за собой: точка, поднятая по просьбе, живёт десять минут, и следующий
    прогон начался бы с чужого хвоста.
    """
    assert metf.post('/wifi', action='ap').status == 202
    state = metf.wait_until(lambda w: w['ap_up'], AP_WAIT_S, what='точка поднята')
    yield state['ap_ssid']
    # Плата могла остаться в другой сети - это дело тестов, не фикстуры
    with contextlib.suppress(OSError):
        metf.post('/wifi', action='ap', value=0)


@pytest.fixture(scope='module')
def phone(atboard: AtBoard, ap: str) -> Phone:
    """AT-плата внутри точки - телефон в руках человека."""
    try:
        ip = atboard.join(ap, '', timeout=JOIN_S)
    except AtError as err:
        pytest.fail(f'AT-плата не вошла в {ap}: {err}')
    assert ip.startswith('192.168.4.'), f'адрес из чужой сети: {ip}'
    return Phone(atboard, HOST)


def test_точка_видна_в_эфире(atboard: AtBoard, ap: str, metf) -> None:
    """
    Точка не просто «поднята» по словам платы, а шлёт маяки.

    Проверка не лишняя: у ESP одно радио, и точка, оставшаяся на старом канале,
    возвращает из softAP() правду, а в эфире молчит.

    Сканируем до тех пор, пока не увидим: один скан - ненадёжный факт. Сразу
    после включения радио AT-плата возвращает обрезанный список (виден один
    сильный сосед и больше ничего), и отсутствие точки в нём говорит о скане,
    а не об эфире.
    """
    seen: dict[str, dict] = {}
    deadline = time.time() + SCAN_WAIT_S
    while ap not in seen:
        with contextlib.suppress(AtError):
            seen = {net['ssid']: net for net in atboard.scan()}
        if ap in seen:
            break
        assert time.time() < deadline, f'{ap} нет в эфире; видно: {sorted(seen)}'
        time.sleep(2.0)

    assert seen[ap]['open'], 'точка должна быть открытой: пароль вводить негде'
    assert seen[ap]['channel'] == metf.wifi()['channel'], (
        'точка в эфире не на том канале, который плата считает своим')


def test_плата_видит_подключившийся_телефон(phone: Phone, metf) -> None:
    state = metf.wait_until(lambda w: w['ap_clients'] >= 1, 15.0,
                            what='плата увидела клиента точки')
    assert state['mode'] == 'ap_sta'


def test_страница_настройки_отдаётся(phone: Phone) -> None:
    answer = phone.get('/')
    assert answer.status == 200, answer.text[:200]
    page = answer.text
    assert '<form' in page and 'name="ssid_manual"' in page, 'на странице нет формы'
    assert 'METF' in page


def test_страница_показывает_сети_рядом(phone: Phone, network) -> None:
    """Список сетей на странице - то, из чего человек выбирает свою."""
    phone.post('/wifi', action='scan')
    ssid = network[0]
    for _ in range(20):
        if ssid in phone.get('/').text:
            return
        time.sleep(1.0)
    pytest.fail(f'сеть {ssid} не появилась в списке на странице')


def test_проба_captive_редиректит_на_страницу(phone: Phone) -> None:
    """
    По этому ответу телефон понимает, что интернета нет, и сам открывает
    страницу настройки. Адрес пробы - Android, но отвечает плата всем.
    """
    answer = phone.get('/generate_204')
    assert answer.status in (301, 302, 307), f'ожидался редирект, пришло {answer.status}'
    assert HOST in answer.headers.get('location', ''), answer.headers


def test_wpad_отдаёт_404(phone: Phone) -> None:
    """Редирект на wpad.dat Windows не устраивает: она спрашивает его вечно."""
    assert phone.get('/wpad.dat').status == 404


def test_состояние_отдаётся_и_внутри_точки(phone: Phone) -> None:
    answer = phone.get('/wifi')
    state = json.loads(answer.text)
    assert state['ap_up'] is True
    assert state['ap_clients'] >= 1
    # Поля с паролем нет; "password" в ответе бывает как причина отказа
    assert [k for k in state if 'pass' in k.lower()] == [], state


@pytest.mark.slow
def test_смена_сети_через_портал(phone: Phone, metf, network) -> None:
    """
    Главное, ради чего всё это: человек с телефона задаёт плате сеть.

    Задаём ту же сеть стенда - плата вернётся туда, где её ждёт стенд, и
    отличие будет видно в source: build -> saved. В конце возвращаем плату на
    вкомпилированную сеть, чтобы следующий прогон начинался с чистого места.
    """
    ssid, password = network
    answer = phone.post('/wifi', ui='1', action='set', ssid=ssid, password=password)
    assert answer.status in (200, 202, 302, 303), answer.text[:200]

    state = metf.wait_until(lambda w: w['connected'] and w['source'] == 'saved',
                            RETURN_WAIT_S, what='плата вернулась в сеть стенда с новыми кредами')
    assert state['ssid'] == ssid
    assert state['ip'] == metf.host, 'адрес платы изменился - стенд её потеряет'

    assert metf.post('/wifi', action='forget').status == 202
    metf.wait_until(lambda w: w['connected'] and w['source'] == 'build',
                    RETURN_WAIT_S, what='плата вернулась на сеть из прошивки')
