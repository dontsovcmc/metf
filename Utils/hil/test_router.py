"""
Беды пользователя на живом роутере: пароль, канал, перезагрузка, нет точки.

Ради этих случаев плата и научилась поднимать свою точку доступа. Здесь они
устраиваются по-настоящему - управляемым роутером стенда (WT32-ETH01,
esp32_nat_router, консоль по проводу), а не подсовыванием плате
несуществующего имени сети.

Сценарии - глазами человека, который перевёз стенд или поменял роутер:

1. Набрал пароль с ошибкой - плата не молчит, а поднимает точку и пишет на
   странице «Неверный пароль».
2. Сменил пароль на роутере - то же самое, но плата до этого была в сети.
3. Выключил роутер и ушёл - плата поднимает точку, а когда роутер включают,
   сама возвращается в сеть; руки для этого не нужны.
4. Перезагрузил роутер - плата пережидает и возвращается, не поднимая точку:
   короткий перерыв не повод звать человека.
5. Роутер переехал на другой канал - плата находит сеть на новом канале.

Плату, ушедшую к управляемому роутеру, рабочая машина не видит: она за NAT.
Поэтому в ту же сеть входит AT-плата и спрашивает METF оттуда - тот же
клиент, что играет телефон в тестах портала.

Два правила, без которых эти тесты врут.

* **Из точки METF надо выходить.** Клиент на точке значит для платы «человек
  настраивает сеть», и пока он там, плата десять минут не делает проб
  (`portal_idle_ms`). Тест, забывший выйти, ждёт возвращения, которого не
  будет, - и обвинит прошивку в том, что она делает правильно.
* **Станцию на точке роутера надо опознавать.** Роутер общий у двух стендов,
  и любая чужая плата в его сети сделала бы «METF вернулась» истинным. Поэтому
  станция считается нашей, только если отвечает по HTTP и зовёт свою точку тем
  же именем, что METF называла в сети стенда.

    pytest Utils/hil --stand -v -k router

Идёт минуты: каждый сценарий ждёт живых таймаутов прошивки.
"""

from __future__ import annotations

import time

import pytest
from conftest import JOIN_S, OWN_AP_HOST, Client, Metf, Phone

from atboard import AtBoard, AtError

pytestmark = pytest.mark.slow

# Опечатка в пароле: плате только что задали сеть, значит «в сети она ещё не
# была», и точку она поднимает сразу после раунда из двух попыток
AP_AFTER_TYPO_S = 90.0
# Плата, которая была в сети и потеряла её: точка через 2 минуты
AP_AFTER_LOST_S = 210.0
# Возвращение в сеть: пробы идут раз в минуту, плюс сама попытка
RETURN_S = 210.0

WRONG_PASSWORD = 'metf-wrong-password'   # длина законная, пароль - нет


class Roaming:
    """
    Плата, которая кочует между сетями, и способ до неё дозваться.

    Стенд смотрит на METF с трёх сторон: из сети стенда (рабочая машина), из
    точки управляемого роутера и из её собственной точки. Где плата сейчас -
    зависит от того, что тест с ней сделал, поэтому адрес не запоминается, а
    ищется заново.
    """

    def __init__(self, metf: Metf, board: AtBoard, router, ap: tuple[str, str]) -> None:
        self.metf = metf
        self.board = board
        self.router = router
        self.ap_ssid, self.ap_password = ap
        # Имя собственной точки платы: по нему она опознаётся и в эфире, и на
        # точке роутера. Снимаем, пока плата в сети стенда.
        self.own_ap = metf.wifi()['ap_ssid']
        self.at_mac = board.mac()

    # --- сеть роутера ---

    def _ours(self, ip: str) -> Client | None:
        """Станция с этим адресом - наша плата? Спрашиваем её саму."""
        view = Phone(self.board, ip)
        try:
            state = view.wifi()
        except (AtError, OSError, ValueError):
            return None
        return view if state.get('ap_ssid') == self.own_ap else None

    def on_router_ap(self) -> Client | None:
        """Плата как клиент управляемого роутера - если она там и если это она."""
        found = self.router.stations(skip=[self.at_mac])
        if not found:
            return None
        self.join_router_ap()
        for station in found:
            view = self._ours(station['ip'])
            if view is not None:
                return view
        return None

    def wait_on_router_ap(self, timeout: float = RETURN_S, what: str = '') -> Client:
        """Дождаться, пока плата вернётся в сеть роутера, и убедиться, что это она."""
        deadline = time.time() + timeout
        while True:
            view = self.on_router_ap()
            if view is not None:
                return view
            if time.time() > deadline:
                raise AssertionError(
                    f'плата не вернулась в сеть роутера за {timeout:.0f} с {what}')
            time.sleep(5.0)

    def join_router_ap(self) -> None:
        """Ввести AT-плату в сеть роутера: оттуда она видит METF за NAT."""
        try:
            self.board.join(self.ap_ssid, self.ap_password, timeout=JOIN_S)
        except AtError as err:
            raise AssertionError(f'AT-плата не вошла в сеть роутера: {err}') from err

    def move_to_router_ap(self) -> Client:
        """
        Перевести плату в сеть управляемого роутера.

        Сеть задаётся так же, как её задаёт человек: сохранением в плату.
        Через неё и возвращаем потом - action=forget.
        """
        view = self.reach()
        assert view.post('/wifi', action='set',
                         ssid=self.ap_ssid, password=self.ap_password).status == 202
        return self.wait_on_router_ap(what='после сохранения сети роутера')

    def ensure_on_router_ap(self) -> Client:
        """Плата в сети роутера - привести её туда, если она не там."""
        view = self.on_router_ap()
        return view if view is not None else self.move_to_router_ap()

    # --- собственная точка платы ---

    def enter_own_ap(self, timeout: float) -> Client:
        """
        Дождаться точки платы в эфире и войти в неё.

        Имя сверяется целиком: рядом может стоять вторая METF, и внутри у
        обеих один и тот же адрес 192.168.4.1.
        """
        deadline = time.time() + timeout
        while True:
            try:
                if any(net['ssid'] == self.own_ap for net in self.board.scan()):
                    self.board.join(self.own_ap, '', timeout=JOIN_S)
                    return Phone(self.board, OWN_AP_HOST)
            except AtError:
                pass          # скан и вход иногда не удаются: пробуем снова
            if time.time() > deadline:
                raise AssertionError(
                    f'плата не подняла свою точку {self.own_ap} за {timeout:.0f} с - '
                    'человеку не к чему подключиться')
            time.sleep(5.0)

    def leave_own_ap(self) -> None:
        """Человек ушёл: пока клиент на точке, плата не пробует сеть."""
        self.board.leave()

    # --- общее ---

    def reach(self, timeout: float = 60.0) -> Client:
        """Дозваться платы с любой стороны. Для восстановления стенда."""
        if self.metf.alive(timeout=3):
            return self.metf
        view = self.on_router_ap()
        if view is not None:
            return view
        return self.enter_own_ap(timeout)

    def back_to_bench(self) -> None:
        """Вернуть плату на вкомпилированную сеть, откуда бы она ни была."""
        self.reach().post('/wifi', action='forget')
        self.leave_own_ap()
        self.metf.wait_until(lambda w: w['connected'] and w['source'] == 'build',
                             RETURN_S, what='плата вернулась в сеть стенда')


@pytest.fixture(scope='module')
def roam(metf: Metf, atboard: AtBoard, router, router_ap) -> Roaming:
    """
    Кочующая плата на весь модуль.

    Возврат в сеть стенда - в конце, а не после каждого теста: перелёт стоит
    минуту, а промежуточное состояние следующему тесту не мешает - сохранение
    сети вытаскивает плату откуда угодно.
    """
    device = Roaming(metf, atboard, router, router_ap)
    yield device
    device.back_to_bench()


def test_пароль_набран_с_ошибкой(roam: Roaming) -> None:
    """
    Человек ошибся в пароле - плата обязана сказать, в чём дело.

    Молчащая плата здесь хуже всего: сеть задана, адрес сменился, и снаружи
    не отличить опечатку от выключенного роутера. Плата поднимает свою точку
    (в сети она ещё не была - ждать две минуты незачем) и пишет причину
    словами: «Неверный пароль», а не «причина 202».
    """
    assert roam.reach().post('/wifi', action='set',
                             ssid=roam.ap_ssid, password=WRONG_PASSWORD).status == 202

    portal = roam.enter_own_ap(AP_AFTER_TYPO_S)
    # Ждём конца раунда: точка могла остаться в эфире от прошлого теста, и
    # первый же ответ застал бы плату на первой попытке
    state = portal.wait_until(lambda w: w['state'] == 'ap' and w['attempts'] >= 2,
                              AP_AFTER_TYPO_S, what='раунд попыток кончился, точка поднята')
    assert state['connected'] is False
    assert state['ssid'] == roam.ap_ssid, 'плата забыла, куда её послали'
    assert state['source'] == 'saved'
    assert state['problem'] == 'password', f'причина разобрана неверно: {state}'

    assert 'Неверный пароль' in portal.get('/').text, 'страница не называет причину человеку'


def test_на_роутере_сменили_пароль(roam: Roaming) -> None:
    """
    Плата была в сети, а пароль на роутере сменили.

    Отличие от опечатки - плата уже работала, и по правилу «две минуты без
    сети» точка поднимается не сразу: короткий сбой не повод звать человека.
    Когда пароль возвращают, плата возвращается сама.
    """
    roam.ensure_on_router_ap()

    with roam.router.password(WRONG_PASSWORD):
        portal = roam.enter_own_ap(AP_AFTER_LOST_S)
        state = portal.wait_until(lambda w: w['problem'] == 'password', 90.0,
                                  what='плата поняла, что дело в пароле')
        assert state['connected'] is False
        assert 'Неверный пароль' in portal.get('/').text
        roam.leave_own_ap()      # человек ушёл, пробы возобновляются

    roam.wait_on_router_ap(what='после возврата пароля')


def test_роутер_выключили_и_включили(roam: Roaming) -> None:
    """
    Главный страх: роутера нет, а плата за стеной.

    Плата поднимает свою точку - человеку есть к чему подключиться. Но если
    роутер включат, она возвращается в сеть сама, без единого нажатия: пока
    точка поднята и к ней никто не подключён, плата раз в минуту пробует
    сеть со сканом.
    """
    roam.ensure_on_router_ap()

    with roam.router.ap_off():
        portal = roam.enter_own_ap(AP_AFTER_LOST_S)
        state = portal.wait_until(lambda w: w['problem'] == 'not_found', 90.0,
                                  what='плата поняла, что сети нет в эфире')
        assert state['ap_up'] is True
        assert 'Сеть не найдена' in portal.get('/').text
        roam.leave_own_ap()

    roam.wait_on_router_ap(what='после включения роутера')


def test_роутер_перезагрузили(roam: Roaming) -> None:
    """
    Перезагрузка роутера - это полминуты, а не авария.

    Плата обязана пережить её молча: вернуться в ту же сеть и не поднимать
    свою точку. Поднятая точка означала бы, что стенд уходит с адреса из-за
    каждого чиха роутера.
    """
    roam.ensure_on_router_ap()

    roam.router.restart()
    view = roam.wait_on_router_ap(what='после перезагрузки роутера')

    state = view.wait_until(lambda w: w['connected'], 60.0, what='плата снова в сети')
    assert state['ap_up'] is False, 'плата подняла точку из-за перезагрузки роутера'
    assert state['ssid'] == roam.ap_ssid


def test_роутер_переехал_на_другой_канал(roam: Roaming) -> None:
    """
    Роутер сменил канал - запомненная пара канал/BSSID промахивается.

    Быстрая попытка идёт на старый канал и проваливается, вторая сканирует
    эфир и находит сеть на новом. Лестницу попыток проверяют хостовые тесты
    политики; здесь важно, что на живом железе плата действительно доезжает
    до нового канала и остаётся доступной.
    """
    view = roam.ensure_on_router_ap()
    was = int(view.wifi()['channel'])
    new = 11 if was <= 6 else 1     # заведомо не соседний: полосы не пересекаются

    with roam.router.channel(new):
        view = roam.wait_on_router_ap(what=f'после переезда на канал {new}')
        state = view.wait_until(lambda w: w['connected'] and int(w['channel']) == new,
                                RETURN_S, what=f'плата нашла сеть на канале {new}')
        assert state['ap_up'] is False, 'переезд канала не должен поднимать точку'
