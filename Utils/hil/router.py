"""
Управляемый роутер стенда: WT32-ETH01 с прошивкой esp32_nat_router.

Нужен, чтобы устроить плате настоящие беды пользователя, а не их имитацию:
сменить пароль точки, увести её на другой канал, выключить, перезагрузить.
Всё это - то, из-за чего METF и научили поднимать свою точку доступа.

Управление идёт по проводу: консоль прошивки слушает TCP 2323 на адресе в
проводной сети. Иначе команда `ap disable` отрезала бы саму себя.

Клиент перенесён из стенда Ватериуса (Utils/hil/router.py того репозитория),
где он ездит по этому же роутеру; здесь оставлено только нужное METF. Грабли,
проверенные там на живой плате, перенесены вместе с кодом:

* Конец ответа определяется по тишине в эфире, а не по приглашению: прошивка
  отвечает на разные команды по-разному, а приглашение печатает не всегда.
* Сразу действует только `ap enable/disable`. `set_ap` и `set_ap_channel`
  ложатся в NVS, а в эфир выходят после `restart`.
* Перезагрузка рвёт сетевую консоль - это не ошибка, а норма: сокет умирает
  вместе с платой. На этом же и проверяется, что перезагрузка случилась.
* Успех команды проверяется по состоянию роутера (`show`), а не по тексту
  ответа: текст свободный, а неузнанный аргумент прошивка иногда проглатывает
  молча.
* Состояние не кэшируется: разъехавшийся стенд иначе даёт зелёный тест на
  неверных настройках.
"""

from __future__ import annotations

import contextlib
import re
import socket
import time
from collections.abc import Iterable, Iterator
from contextlib import contextmanager

CONSOLE_PORT = 2323

IDLE_GAP = 0.25          # тишина, по которой судим, что ответ кончился
CMD_TIMEOUT = 4.0
SHOW_TIMEOUT = 6.0

# Перезагрузка: консоль отвечает через 5-6 с, клиент выходит наружу через 7-9 с
# (замеры стенда Ватериуса, 03_router-nat-bug.md). 12 с - с запасом.
RESTART_WAIT = 12.0
# За столько консоль обязана замолчать, если плата и правда ушла в ребут
RESTART_DIES_IN = 8.0

ERROR_MARKERS = (
    'Unrecognized command',
    'Command returned non-zero',
    'Invalid arguments',
    # Разбор аргументов ругается своими словами; без этих двух строк ошибка
    # проглатывалась, и тест считал невыполненную команду выполненной
    'missing option',
    'excess option',
)


class RouterError(RuntimeError):
    """Роутер не принял команду или ответил ошибкой."""


class Console:
    """Сетевая консоль прошивки: TCP, пароль строкой сразу после подключения."""

    def __init__(self, host: str, password: str, port: int = CONSOLE_PORT) -> None:
        self._host = host
        self._password = password
        self._port = port
        self._sock: socket.socket | None = None
        self._connect()

    def _connect(self) -> None:
        sock = socket.create_connection((self._host, self._port), timeout=5.0)
        sock.settimeout(0.2)
        time.sleep(0.3)
        sock.sendall(self._password.encode() + b'\r\n')
        time.sleep(0.3)
        self._sock = sock
        if 'Authentication failed' in self.drain():
            self.close()
            raise RouterError('консоль роутера не приняла пароль ([router] password)')

    def reconnect(self, timeout: float = 60.0) -> None:
        """
        Поднять консоль заново после перезагрузки платы.

        Порт 2323 принимает соединение раньше, чем консоль готова отвечать,
        поэтому готовность здесь не измеряется - её проверяет контрольная
        команда в Router.restart().
        """
        self.close()
        deadline = time.time() + timeout
        while True:
            try:
                self._connect()
                return
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(2.0)

    def alive(self) -> bool:
        """Жив ли сокет: перезагрузка платы его рвёт, и это наш признак ребута."""
        if self._sock is None:
            return False
        try:
            self._sock.sendall(b'\r\n')
            return True
        except OSError:
            return False

    def _send(self, line: str) -> None:
        assert self._sock is not None
        self._sock.sendall(line.encode() + b'\r\n')

    def write_line(self, line: str) -> None:
        """
        Отправить команду, подняв консоль заново, если она успела оборваться.

        Плата закрывает сокет и сама по себе. Повтор здесь безопасен: раз
        отправить не удалось, до роутера не дошло ничего.
        """
        try:
            self._send(line)
        except OSError:
            self.reconnect()
            self._send(line)

    def read_idle(self, timeout: float, idle: float = IDLE_GAP) -> str:
        assert self._sock is not None
        deadline = time.time() + timeout
        chunks: list[bytes] = []
        last = time.time()
        while time.time() < deadline:
            try:
                data = self._sock.recv(4096)
            except socket.timeout:
                data = b''
            except OSError as err:
                # Консоль умерла на середине ответа. Отдать обрывок - значит
                # соврать о состоянии роутера; поднимаем сокет ради следующих
                # команд и говорим прямо.
                self.reconnect()
                raise RouterError('консоль оборвалась на середине ответа, '
                                  'команду нужно повторить') from err
            if data:
                chunks.append(data)
                last = time.time()
            elif chunks and time.time() - last >= idle:
                break
        return b''.join(chunks).decode(errors='replace')

    def drain(self) -> str:
        """Выбросить всё, что осталось от прошлой команды, и вернуть выброшенное."""
        if self._sock is None:
            return ''
        out = b''
        try:
            while True:
                data = self._sock.recv(4096)
                if not data:
                    break
                out += data
        except (socket.timeout, OSError):
            pass
        return out.decode(errors='replace')

    def close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None


class Router:
    """Точка доступа стенда, которой можно устроить беду по команде."""

    def __init__(self, console: Console, ap_password: str = '') -> None:
        self._c = console
        # Пароль точки у роутера не прочитать: `show config` печатает
        # звёздочки. Без него не вернуть точку в исходное состояние - поэтому
        # он приходит из stand.ini.
        self.ap_password = ap_password
        # Трогали ли мы точку: снимок настроек про пароль ничего не знает, и
        # вернуть его можно только по этой памятке
        self._ap_dirty = False

    # --- основа ---

    def cmd(self, line: str, timeout: float = CMD_TIMEOUT) -> str:
        self._c.drain()
        self._c.write_line(line)
        out = self._c.read_idle(timeout)
        for marker in ERROR_MARKERS:
            if marker in out:
                raise RouterError(f'{line!r}: {marker}\n{out.strip()}')
        return _strip_echo(out, line)

    def version(self) -> str:
        return self.cmd('version').strip()

    def show(self, section: str = 'config') -> str:
        return self.cmd(f'show {section}', timeout=SHOW_TIMEOUT)

    def config(self) -> dict[str, str]:
        """`show config` в словарь; ключи - как их печатает прошивка."""
        return _parse_kv(self.show('config'))

    def ap_ssid(self) -> str:
        ssid = self.config().get('ssid', '')
        if not ssid:
            raise RouterError('роутер не сообщил имя своей точки (show config)')
        return ssid

    def ap_channel(self) -> int:
        """Канал из настроек. В эфир он выходит только после restart()."""
        return _number(self.config().get('channel', ''), default=0)

    # --- точка доступа ---

    def ap_enabled(self) -> bool | None:
        m = re.search(r'AP interface:\s*(\w+)', self.show('status'))
        return None if m is None else m.group(1).lower() == 'enabled'

    def ap(self, enabled: bool) -> None:
        """
        Включить или выключить точку - единственная команда, которая действует
        сразу, без перезагрузки.

        Успех судим по состоянию: текст ответа у прошивки свободный, и
        неузнанный аргумент она проглатывает молча.
        """
        self._ap_dirty = True
        self.cmd('ap enable' if enabled else 'ap disable')
        if self.ap_enabled() is not enabled:
            raise RouterError(f'точка доступа не {"включилась" if enabled else "выключилась"}')

    def set_ap(self, ssid: str, password: str) -> None:
        """Имя и пароль точки. В эфир выходят после restart()."""
        self._ap_dirty = True
        self.cmd(f'set_ap {_quoted(ssid)} {_quoted(password)}')

    def set_ap_channel(self, channel: int) -> None:
        """0 - авто, 1..13 - фиксированный. В эфир выходит после restart()."""
        self._ap_dirty = True
        self.cmd(f'set_ap_channel {channel}')

    def restart(self, wait: float = RESTART_WAIT) -> None:
        """
        Перезагрузить плату и дождаться живой консоли.

        Факт перезагрузки проверяется, а не предполагается: `set_ap` и
        `set_ap_channel` без неё остаются в NVS, и тест, поверивший на слово,
        ищет беду, которой не случилось. Признак - смерть сокета: команда
        уходит по сети, и сеть умирает вместе с платой.
        """
        with contextlib.suppress(OSError):   # плата успела уйти в перезагрузку
            self._c.write_line('restart')

        deadline = time.time() + RESTART_DIES_IN
        while self._c.alive():
            if time.time() > deadline:
                raise RouterError('роутер не перезагрузился: консоль отвечает как ни в чём '
                                  'не бывало, а настройки без ребута остаются в NVS')
            time.sleep(0.5)

        time.sleep(wait)
        self._c.reconnect()
        self.version()      # консоль отвечает, а не просто принимает соединение

    # --- клиенты ---

    def stations(self, skip: Iterable[str] = ()) -> list[dict[str, str]]:
        """
        Станции на точке: mac, ip. Кроме перечисленных по MAC - обычно кроме
        самого стенда.
        """
        ignore = {mac.lower() for mac in skip}
        found = []
        for line in self.show('status').splitlines():
            m = re.search(r'((?:[0-9a-f]{2}:){5}[0-9a-f]{2})\s+(\d+\.\d+\.\d+\.\d+)',
                          line, re.IGNORECASE)
            if m and m.group(1).lower() not in ignore:
                found.append({'mac': m.group(1).lower(), 'ip': m.group(2)})
        return found

    # --- сценарии ---

    @contextmanager
    def ap_off(self) -> Iterator[None]:
        """
        Точки нет в эфире: роутер выключили или унесли.

        Возврат идёт с перезагрузкой - обход ошибки прошивки: в сборке с
        проводным аплинком NAT после `ap enable` не возвращается, и клиенты
        точки видят только её саму.
        """
        self.ap(False)
        try:
            yield
        finally:
            self.ap(True)
            self.restart()

    @contextmanager
    def channel(self, number: int) -> Iterator[int]:
        """Роутер переехал на другой канал."""
        was = self.ap_channel()
        self.set_ap_channel(number)
        self.restart()
        try:
            yield number
        finally:
            self.set_ap_channel(was)
            self.restart()

    @contextmanager
    def password(self, new: str) -> Iterator[str]:
        """Пароль точки сменили, имя осталось прежним."""
        ssid = self.ap_ssid()
        self.set_ap(ssid, new)
        self.restart()
        try:
            yield ssid
        finally:
            self.set_ap(ssid, self._known_password())
            self.restart()

    # --- снимок и возврат ---

    def snapshot(self) -> dict[str, str]:
        return self.config()

    def restore(self, state: dict[str, str]) -> None:
        """
        Вернуть роутер к снимку.

        Имя и пароль ставятся заново без сравнения: пароль в `show config` -
        звёздочки, сравнить его не с чем, а роутер, оставшийся с подсунутым
        паролем, положит все следующие прогоны, обвинив плату. Цена ошибки
        несравнимо выше одной лишней перезагрузки. Перезагрузка нужна и сама
        по себе: после `ap disable` NAT оживает только ею.
        """
        if not self._ap_dirty:
            return

        self.ap(True)
        ssid = state.get('ssid', '')
        if ssid:
            self.set_ap(ssid, self._known_password())
        self.set_ap_channel(_number(state.get('channel', ''), default=0))
        self.restart()
        self._ap_dirty = False

    def _known_password(self) -> str:
        if not self.ap_password:
            raise RouterError('нужен [router] ap_password в stand.ini: пароль точки '
                              'у роутера не прочитать, show config печатает звёздочки')
        return self.ap_password

    def close(self) -> None:
        self._c.close()


def connect(host: str, password: str, ap_password: str = '') -> Router:
    return Router(Console(host, password), ap_password)


def _strip_echo(out: str, line: str) -> str:
    """Консоль повторяет введённую команду - убираем, чтобы не мешала разбору."""
    lines = out.splitlines()
    if lines and lines[0].strip().endswith(line.strip()):
        lines = lines[1:]
    return '\n'.join(lines).strip()


def _quoted(value: str) -> str:
    """Аргумент консоли в кавычках: пробел внутри иначе делит его надвое."""
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


def _number(value: str, default: int) -> int:
    """Первое число в значении: канал прошивка печатает и как «1», и как «1 (auto)»."""
    m = re.search(r'\d+', value)
    return int(m.group()) if m else default


def _parse_kv(text: str) -> dict[str, str]:
    """
    `show` печатает пары «ключ: значение» и «ключ = значение».

    Разбираем оба, ключ приводим к snake_case - по нему сверяется снимок.
    """
    result: dict[str, str] = {}
    for line in text.splitlines():
        m = re.match(r'\s*([A-Za-z][A-Za-z0-9 _.-]*?)\s*[:=]\s*(.*?)\s*$', line)
        if not m:
            continue
        key = re.sub(r'[ .-]+', '_', m.group(1).strip()).lower()
        result[key] = m.group(2)
    return result
