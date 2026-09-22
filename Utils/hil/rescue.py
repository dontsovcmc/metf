"""
Вернуть плату домой, где бы она ни была.

Прогон, упавший посередине, оставляет METF в чужой сети - в точке
управляемого роутера или в собственной точке. Рабочая машина её оттуда не
видит, и следующий прогон целиком пропускается со словами «плата не
отвечает». Этот скрипт ищет плату всеми тремя путями и возвращает на
вкомпилированную сеть (`action=forget`).

    python3 Utils/hil/rescue.py

Адреса и порты - из stand.ini, как и у тестов.
"""

from __future__ import annotations

import configparser
import contextlib
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from conftest import Client, Metf, Phone

import router as router_mod
from atboard import AtBoard, AtError

OWN_AP_HOST = '192.168.4.1'
PORTMAP_PORT = 8080
RETURN_S = 210.0


def find(metf: Metf, board: AtBoard, cfg: configparser.ConfigParser) -> Client | None:
    """Плата в сети стенда, в сети роутера или в собственной точке."""
    if metf.alive(timeout=3):
        print(f'плата в сети стенда: {metf.host}')
        return metf

    host = cfg.get('router', 'host', fallback='')
    if host:
        try:
            device = router_mod.connect(host, cfg.get('router', 'password', fallback=''),
                                        cfg.get('router', 'ap_password', fallback=''))
        except (OSError, router_mod.RouterError) as err:
            print(f'консоль роутера {host} молчит: {err}')
        else:
            # Плату за NAT видно через проброс порта, а не через эфир: телефон
            # стенда этот роутер слышит через раз
            with device:
                for station in device.stations(skip=[board.mac()]):
                    device.portmaps_clear()
                    device.portmap_add(PORTMAP_PORT, station['ip'])
                    view = Metf(f'{host}:{PORTMAP_PORT}')
                    with contextlib.suppress(OSError, ValueError):
                        if view.wifi():
                            print(f'плата в сети роутера: {station["ip"]}')
                            return view
                device.portmaps_clear()

    print('ищу собственную точку платы в эфире...')
    deadline = time.time() + 120
    while time.time() < deadline:
        with contextlib.suppress(AtError):
            for net in board.scan():
                if net['ssid'].startswith('METF-'):
                    board.join(net['ssid'], '', timeout=60)
                    print(f'плата в своей точке: {net["ssid"]}')
                    return Phone(board, OWN_AP_HOST)
        time.sleep(5.0)
    return None


def main() -> int:
    path = Path(__file__).parent / 'stand.ini'
    if not path.exists():
        print('нет Utils/hil/stand.ini, см. stand.ini.example')
        return 2
    cfg = configparser.ConfigParser()
    cfg.read(path)

    metf = Metf(cfg.get('metf', 'host'))
    board = AtBoard(cfg.get('atboard', 'port'))
    try:
        view = find(metf, board, cfg)
        if view is None:
            print('плату не найти: ни в сети стенда, ни на роутере, ни в её точке')
            return 1
        if view is metf and metf.wifi().get('source') == 'build':
            print('плата уже дома, делать нечего')
            return 0
        print('возвращаю на сеть из прошивки:', view.post('/wifi', action='forget').status)
        board.leave()
    finally:
        board.close()

    state = metf.wait_until(lambda w: w['connected'] and w['source'] == 'build',
                            RETURN_S, what='плата вернулась в сеть стенда')
    print(f'готово: {state["ip"]}, сеть {state["ssid"]}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
