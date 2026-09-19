"""
Порт для заливки выбирается по USB-серийнику платы, а не по имени устройства.

Имя вида `/dev/cu.usbmodem2101` принадлежит не плате, а разъёму: переткнул
кабель - и то же имя достаётся соседней плате. В `platformio.ini` такое имя
лежало годами и успело устареть, а на стенде рядом с METF постоянно висят ещё
две платы.

Убрать имя из конфига и довериться автоопределению нельзя: PlatformIO ищет
первый порт, чей VID:PID знаком хоть одной установленной платформе, и на этом
стенде находит CP2102 - плату AT, а не METF. Заливка молча ушла бы в чужую
плату.

USB-серийник у платы свой: у ESP32 это MAC (его же плата печатает в консоль при
загрузке), у CP2102 - строка из EEPROM. `pio device list` показывает его как
`SER=`. Привязка серийника к окружению лежит в `secrets.ini` - файле, который у
каждой машины свой и в git не попадает:

    [board_serial]
    esp32-c6-super-mini = REDACTED
    nodemcuv2 = 0001

Если привязки нет, а плат подключено несколько - сборка останавливается со
списком портов. Лучше остановиться, чем прошить не ту плату.
"""

import configparser
import os

from SCons.Script import BUILD_TARGETS, COMMAND_LINE_TARGETS
from serial.tools import list_ports

Import("env")                                            # noqa: F821

UPLOAD_TARGETS = ('upload', 'uploadfs', 'uploadfsota')


def configured_serial(project_dir: str, env_name: str) -> str:
    """Серийник платы для этого окружения: переменная среды или `secrets.ini`."""
    from_env = os.environ.get('METF_BOARD_SERIAL', '').strip()
    if from_env:
        return from_env
    config = configparser.ConfigParser()
    config.read(os.path.join(project_dir, 'secrets.ini'))
    return config.get('board_serial', env_name, fallback='').strip()


def candidates() -> list:
    """Порты, за которыми может стоять плата: у Bluetooth серийника нет."""
    return [port for port in list_ports.comports() if port.serial_number]


def describe(ports: list) -> str:
    return '\n'.join(f'    {port.device}  SER={port.serial_number}  {port.description}'
                     for port in ports) or '    (ни одного)'


def pick(wanted: str, ports: list) -> str:
    """Порт платы с таким серийником. Регистр не важен: MAC пишут по-разному."""
    for port in ports:
        if port.serial_number.upper() == wanted.upper():
            return port.device
    return ''


def main() -> None:
    if not set(UPLOAD_TARGETS) & set(COMMAND_LINE_TARGETS + BUILD_TARGETS):
        return                                           # сборка без заливки

    if env.subst('$UPLOAD_PORT'):                        # noqa: F821
        return                                           # порт задан явно

    project_dir = env.subst('$PROJECT_DIR')              # noqa: F821
    env_name = env['PIOENV']                             # noqa: F821
    ports = candidates()
    wanted = configured_serial(project_dir, env_name)

    if wanted:
        found = pick(wanted, ports)
        if not found:
            raise SystemExit(
                f'Плата с серийником {wanted} (окружение {env_name}) не найдена.\n'
                f'Подключены:\n{describe(ports)}')
        print(f'Порт по серийнику {wanted}: {found}')
        env.Replace(UPLOAD_PORT=found)                   # noqa: F821
        return

    if len(ports) > 1:
        raise SystemExit(
            f'Подключено несколько плат, а какая нужна окружению {env_name} - не сказано.\n'
            f'{describe(ports)}\n'
            'Укажите порт: pio run -t upload --upload-port <порт>\n'
            'или пропишите серийник один раз в secrets.ini:\n'
            f'    [board_serial]\n    {env_name} = <SER из списка выше>')


main()
