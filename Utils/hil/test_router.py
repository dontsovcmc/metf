"""
Беды пользователя на живом роутере: пароль, канал, перезагрузка, нет точки.

Ради этих случаев плата и научилась поднимать свою точку доступа. Здесь они
устраиваются по-настоящему - управляемым роутером стенда (WT32-ETH01,
esp32_nat_router, консоль по проводу), а не подсовыванием плате
несуществующего имени сети.

Сценарии - глазами человека, который перевёз стенд или поменял роутер:

1. Набрал пароль с ошибкой - плата не молчит, а поднимает точку и пишет на
   странице «Неверный пароль».
2. Сменил пароль на роутере - плата, которая была в сети, через две минуты
   поднимает точку, объясняет причину, и человек набирает новый пароль в ту же
   форму; плата возвращается в сеть.
3. Переименовал сеть - плата не находит её, а новое имя человек не набирает
   вслепую: оно есть в списке сетей на странице, найденное сканом платы.
4. Выключил роутер и ушёл - плата поднимает точку, а когда роутер включают,
   сама возвращается в сеть; руки для этого не нужны.
5. Перезагрузил роутер - плата пережидает и возвращается, не поднимая точку:
   короткий перерыв не повод звать человека.
6. Роутер переехал на другой канал - плата находит сеть на новом канале.

Плату, ушедшую к управляемому роутеру, рабочая машина не видит: она за NAT.
Поэтому стенд просит роутер пробросить её порт наружу, на проводную сторону,
и говорит с платой оттуда. Посредником в эфире это не делается сознательно:
управляемый роутер стоит далеко, плата живёт на нём при -84 дБм, а телефон
стенда (ESP8266) его и вовсе слышит через раз. AT-плата занята тем, что у неё
получается отлично, - собственной точкой METF в двадцати сантиметрах.

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
# Возвращение в сеть. Складывается из того, что прошивке положено делать:
# до 30 с плата замечает пропажу (beacon timeout), 120 с идёт раундами, не
# поднимая точку, потом раз в минуту делает пробу со сканом, и сама попытка с
# получением адреса - ещё секунд пятнадцать. Худший случай - 225 с, и прежние
# 210 обрезали его ровно там, где плата ещё работает по правилам: на этом и
# падал переезд канала, где быстрая попытка заведомо мимо. Берём с запасом.
RETURN_S = 300.0
# Сколько ждём плату по адресу стенда, прежде чем искать её в чужих сетях
BENCH_WAIT_S = 30.0

# Сколько ждём, пока плата разберётся, в чём беда: причина уточняется от
# попытки к попытке, а первым делом ядро говорит только «связь оборвалась»
PROBLEM_S = 120.0
SCAN_S = 60.0     # от «обновить список» до новой сети на странице

# Порт на проводной стороне роутера, за которым видна плата, ушедшая за NAT
PORTMAP_PORT = 8080
# Через проброс разговор идёт дольше: запрос проходит NAT роутера, а сама
# плата сидит на нём при -84 дБм. Пяти секунд «как в сети стенда» не хватает -
# на них падал `forget`, которым тест возвращает плату домой.
PORTMAP_TIMEOUT_S = 20.0

# Куда уводим роутер в сценарии с переездом. Канал выбран замером, а не
# «подальше по полосе»: роутер стоит далеко, и плата слышит его на канале 11
# в нуле сканов из двух (там сосед), на канале 1 - в одном из двух, на 9 -
# в двух из двух. Прежний выбор «11, раз сейчас 6» давал тест, падавший на
# физике: плата честно сканировала эфир, а сети в нём для неё не было. Выше
# 11 стенд роутер не уводит вовсе - см. Router.set_ap_channel().
MOVE_TO_CHANNEL = 9
HOME_CHANNEL = 6      # куда уводим, если роутер уже стоит на MOVE_TO_CHANNEL

WRONG_PASSWORD = 'metf-wrong-password'   # длина законная, пароль - нет
NEW_PASSWORD = 'metf-stand-new-pass'     # его человек и набирает в портале
NEW_SSID = 'metf-stand-renamed'          # имя, под которым точка «исчезает»


class Roaming:
    """
    Плата, которая кочует между сетями, и способ до неё дозваться.

    Стенд смотрит на METF с трёх сторон: из сети стенда (рабочая машина), из
    точки управляемого роутера и из её собственной точки. Где плата сейчас -
    зависит от того, что тест с ней сделал, поэтому адрес не запоминается, а
    ищется заново.
    """

    def __init__(self, metf: Metf, board: AtBoard, router, ap: tuple[str, str],
                 router_host: str, bench: tuple[str, str]) -> None:
        self.metf = metf
        self.board = board
        self.router = router
        self.ap_ssid, self.ap_password = ap
        self.bench_ssid, self.bench_password = bench
        # Адрес, по которому плата видна за NAT: проброс порта на проводной
        # стороне роутера
        self.through_router = Metf(f'{router_host}:{PORTMAP_PORT}', PORTMAP_TIMEOUT_S)
        # Имя собственной точки платы: по нему она опознаётся и в эфире, и на
        # точке роутера. Снимаем, пока плата в сети стенда.
        self.own_ap = metf.wifi()['ap_ssid']
        self.at_mac = board.mac()

    # --- сеть роутера ---

    def _ours(self, ip: str) -> Client | None:
        """
        Станция с этим адресом - наша плата? Спрашиваем её саму.

        Проброс порта ставим заново каждый раз, без памяти о том, что он уже
        стоял: уборка после теста снимает все пробросы, и запомненный «уже
        стоит» делал плату невидимой - стенд искал её в эфире, хотя она
        спокойно сидела на точке роутера. Два лишних вызова консоли дешевле
        такой слепоты.
        """
        self.router.portmaps_clear()
        self.router.portmap_add(PORTMAP_PORT, ip)
        try:
            state = self.through_router.wifi()
        except (OSError, ValueError):
            return None
        return self.through_router if state.get('ap_ssid') == self.own_ap else None

    def on_router_ap(self) -> Client | None:
        """Плата как клиент управляемого роутера - если она там и если это она."""
        for station in self.router.stations(skip=[self.at_mac]):
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

    def move_to_router_ap(self) -> Client:
        """
        Перевести плату в сеть управляемого роутера.

        Сеть задаётся так же, как её задаёт человек: сохранением в плату.
        Через неё и возвращаем потом - action=forget.
        """
        view = self.reach()
        assert view.command('/wifi', action='set',
                            ssid=self.ap_ssid, password=self.ap_password).status == 202
        return self.wait_on_router_ap(what='после сохранения сети роутера')

    def ensure_on_router_ap(self) -> Client:
        """
        Плата в сети роутера - привести её туда, если она не там.

        Сперва уводим AT-плату из точки METF: клиент на точке и держит её
        поднятой (`ap_stop_grace`), и глушит пробы. Прошлый тест мог оставить
        там «телефон», и следующий увидел бы поднятую точку как беду.
        """
        self.leave_own_ap()
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

    def reach(self, timeout: float = 150.0) -> Client:
        """
        Дозваться платы с любой стороны. Для восстановления стенда.

        Сеть стенда спрашиваем не один раз, а полминуты: связь тут слабая
        (-67..-84 дБм), плата регулярно переподключается, и один неудачный
        /ping не значит, что она ушла в другую сеть. Без этого тест начинал
        искать в эфире точку, которую плата и не собиралась поднимать, - и
        падал на ровном месте.
        """
        deadline = time.time() + BENCH_WAIT_S
        while True:
            if self.metf.alive(timeout=3):
                return self.metf
            view = self.on_router_ap()
            if view is not None:
                return view
            if time.time() > deadline:
                break
            time.sleep(3.0)
        return self.enter_own_ap(timeout)

    def send_to_bench(self, view: Client) -> None:
        """
        Вернуть плату на вкомпилированную сеть через уже открытый вид на неё.

        Тест, который менял настройки роутера, обязан увести плату отсюда до
        того, как настройки вернут: иначе плата останется с сохранённым
        паролем, которого у роутера уже нет, и следующий тест начнётся с
        двухминутного ожидания её точки.
        """
        assert view.command('/wifi', action='forget').status == 202
        self.leave_own_ap()
        self.metf.wait_until(lambda w: w['connected'] and w['source'] == 'build',
                             RETURN_S, what='плата вернулась в сеть стенда')

    def back_to_bench(self) -> None:
        """Вернуть плату на вкомпилированную сеть, откуда бы она ни была."""
        self.send_to_bench(self.reach())


@pytest.fixture(scope='module')
def roam(metf: Metf, atboard: AtBoard, router, router_ap, stand, network) -> Roaming:
    """Кочующая плата на весь модуль."""
    device = Roaming(metf, atboard, router, router_ap, stand.get('router', 'host'), network)
    yield device
    device.back_to_bench()


@pytest.fixture(autouse=True)
def clean_stand(roam: Roaming, router):
    """
    После каждого теста - роутер к исходным настройкам, плата домой.

    Без этого один упавший тест уносит за собой все следующие: роутер
    остаётся с подсунутым паролем или чужим именем, плата - с сетью, в
    которую ей не войти, и дальше всё сыпется по чужой вине. Так и вышло:
    падение первого сценария положило четыре следующих.

    Стоит это перезагрузки роутера и перелёта платы - около минуты на тест.
    Дешевле, чем разбираться в каскаде.
    """
    before = router.snapshot()
    yield
    router.restore(before)
    roam.leave_own_ap()
    if not roam.metf.alive(timeout=3):
        roam.back_to_bench()


def test_пароль_набран_с_ошибкой(roam: Roaming) -> None:
    """
    Человек ошибся в пароле - плата обязана сказать, в чём дело.

    Молчащая плата здесь хуже всего: сеть задана, адрес сменился, и снаружи
    не отличить опечатку от выключенного роутера. Плата поднимает свою точку
    (в сети она ещё не была - ждать две минуты незачем) и пишет причину
    словами: «Неверный пароль», а не «причина 202».

    Сеть берём стендовую, а не управляемого роутера, и вот почему. Тот стоит
    далеко: плата слышит его на -84 дБм и при неверном пароле сдаётся раньше
    рукопожатия - ядро отдаёт 201 NO_AP_FOUND, и портал честно пишет «Сеть не
    найдена». Отказ по паролю виден только там, где связь уверенная, - на
    слабой связи «неверный пароль» неотличим от «сети нет», и это свойство
    радио, а не прошивки.
    """
    assert roam.reach().command('/wifi', action='set', ssid=roam.bench_ssid,
                                password=WRONG_PASSWORD).status == 202

    portal = roam.enter_own_ap(AP_AFTER_TYPO_S)
    # Ждём конца раунда: точка могла остаться в эфире от прошлого теста, и
    # первый же ответ застал бы плату на первой попытке. Признак - поднятая
    # точка, а не state: в точке политика раз в минуту уходит пробовать сеть,
    # и state на это время становится connecting, хотя точка в эфире стоит.
    state = portal.wait_until(lambda w: w['ap_up'] and w['attempts'] >= 2,
                              AP_AFTER_TYPO_S, what='раунд попыток кончился, точка поднята')
    assert state['connected'] is False
    assert state['ssid'] == roam.bench_ssid, 'плата забыла, куда её послали'
    assert state['source'] == 'saved'
    assert state['problem'] == 'password', f'причина разобрана неверно: {state}'

    assert 'Неверный пароль' in portal.get('/').text, 'страница не называет причину человеку'

    # Возвращаем плату домой через портал - как это сделал бы человек
    roam.send_to_bench(portal)


def test_на_роутере_сменили_пароль(roam: Roaming) -> None:
    """
    Пароль сменили на роутере - и человек вводит новый в плату.

    Полный путь беды: плата работала, связь пропала, через две минуты без сети
    поднялась точка (короткий сбой не повод звать человека), на странице
    написано, в чём дело, человек набирает новый пароль - и плата снова в
    сети. Ровно это и делают, когда меняют пароль на роутере.

    Причина здесь проверяется мягче, чем в тесте про опечатку: годится и
    «неверный пароль», и «сеть не найдена». Управляемый роутер стоит далеко,
    плата слышит его на -84 дБм и на таком уровне сдаётся раньше рукопожатия -
    ядро отдаёт 201 NO_AP_FOUND. Требовать именно `password` значило бы
    требовать от прошивки различать то, чего в эфире нет. Что причина
    называется словами и берётся не с потолка, проверяет соседний тест на
    уверенной связи; здесь важно другое - плата не молчит и возвращается в
    сеть с новым паролем.
    """
    roam.ensure_on_router_ap()

    with roam.router.password(NEW_PASSWORD) as ssid:
        portal = roam.enter_own_ap(AP_AFTER_LOST_S)
        state = portal.wait_until(lambda w: w['problem'] in ('password', 'not_found'),
                                  PROBLEM_S, what='плата назвала беду словами')
        assert state['connected'] is False
        page = portal.get('/').text
        assert 'Неверный пароль' in page or 'Сеть не найдена' in page, (
            'страница не называет причину человеку')

        # Человек набирает новый пароль в ту же форму, что и на телефоне
        answer = portal.post('/wifi', ui='1', action='set', ssid=ssid, password=NEW_PASSWORD)
        assert answer.status in (200, 202, 302, 303), answer.text[:200]
        roam.leave_own_ap()      # человек ушёл со страницы

        view = roam.wait_on_router_ap(what='с новым паролем')
        back = view.wait_until(lambda w: w['connected'], 60.0, what='плата снова в сети')
        assert back['ssid'] == ssid
        assert back['problem'] == 'none', 'плата в сети, а причина отказа не убрана'

        # Увести плату домой, пока пароль на роутере ещё новый: иначе она
        # останется с паролем, которого у роутера уже нет
        roam.send_to_bench(view)


def test_на_роутере_сменили_имя_сети(roam: Roaming) -> None:
    """
    Точку переименовали - для платы сеть просто исчезла.

    Человеку это чинится только через портал, и именно так, как задумано:
    новое имя он не набирает вслепую, а видит в списке сетей на странице -
    его плата нашла сканом. Выбрал, ввёл пароль - и снова в сети.
    """
    roam.ensure_on_router_ap()

    with roam.router.renamed(NEW_SSID) as ssid:
        portal = roam.enter_own_ap(AP_AFTER_LOST_S)
        state = portal.wait_until(lambda w: w['problem'] == 'not_found', PROBLEM_S,
                                  what='плата поняла, что прежней сети нет в эфире')
        assert state['connected'] is False
        assert 'Сеть не найдена' in portal.get('/').text

        # «Обновить список» на странице - и новое имя должно в нём появиться
        portal.post('/wifi', ui='1', action='scan')
        deadline = time.time() + SCAN_S
        while ssid not in portal.get('/').text:
            assert time.time() < deadline, (
                f'новое имя {ssid} не появилось в списке сетей на странице - '
                'человеку не из чего выбирать')
            time.sleep(3.0)

        answer = portal.post('/wifi', ui='1', action='set', ssid=ssid,
                             password=roam.ap_password)
        assert answer.status in (200, 202, 302, 303), answer.text[:200]
        roam.leave_own_ap()

        view = roam.wait_on_router_ap(what='с новым именем сети')
        back = view.wait_until(lambda w: w['connected'], 60.0, what='плата снова в сети')
        assert back['ssid'] == ssid
        assert back['problem'] == 'none'

        roam.send_to_bench(view)


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
    new = MOVE_TO_CHANNEL if was != MOVE_TO_CHANNEL else HOME_CHANNEL

    with roam.router.channel(new):
        view = roam.wait_on_router_ap(what=f'после переезда на канал {new}')
        state = view.wait_until(lambda w: w['connected'] and int(w['channel']) == new,
                                RETURN_S, what=f'плата нашла сеть на канале {new}')
        assert state['ap_up'] is False, 'переезд канала не должен поднимать точку'
