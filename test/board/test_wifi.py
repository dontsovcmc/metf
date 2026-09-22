"""Состояние сети платы: GET /wifi и проверки POST /wifi.

Опасное не трогаем: set и forget уводят плату с сети стенда, а тест,
меняющий сеть под собой, чинится руками у платы. Проверяется то, что
можно проверить, не трогая подключение.
"""

import time


def test_wifi_status_says_connected(board):
    """Плата, с которой мы говорим по сети, обязана считать себя в сети."""
    st = board.get_json("/wifi")
    assert st["connected"] is True, st
    assert st["state"] == "online", st
    assert st["ip"] == board.host, st


def test_wifi_status_has_no_password(board):
    """Пароль наружу не отдаётся никогда."""
    body = board.get("/wifi").lower()
    assert "password" not in body
    assert "pass" not in body


def test_wifi_status_fields(board):
    st = board.get_json("/wifi")
    for key in ("mode", "ssid", "source", "rssi", "channel", "fast",
                "ap_ssid", "ap_up", "ap_clients", "offline_s", "attempts",
                "last_reason", "hw_error", "pending", "scanning"):
        assert key in st, f"нет поля {key}: {st}"
    assert st["source"] in ("build", "saved"), st
    assert 1 <= st["channel"] <= 13, st
    assert st["hw_error"] is False, "плата сообщает об ошибке железа"


def test_post_wifi_without_action(board):
    code, body = board.post_raw("/wifi", {})
    assert code == 400, (code, body)
    assert "action" in body


def test_post_wifi_unknown_action(board):
    code, body = board.post_raw("/wifi", {"action": "bogus"})
    assert code == 400, (code, body)


def test_post_wifi_set_rejects_empty_ssid(board):
    """Пустое имя сети увело бы плату в никуда - отказ до сохранения."""
    code, body = board.post_raw("/wifi", {"action": "set", "ssid": "", "password": ""})
    assert code == 400, (code, body)
    assert "ssid" in body


def test_post_wifi_set_rejects_short_password(board):
    code, body = board.post_raw(
        "/wifi", {"action": "set", "ssid": "some-network", "password": "1234"}
    )
    assert code == 400, (code, body)
    assert "password" in body


def test_portal_page_is_served(board):
    page = board.get("/")
    assert "<form" in page
    assert 'name="ssid_manual"' in page
    assert board.host in page, "на странице должен быть адрес платы"


def test_scan_command_accepted(board):
    """Скан принимается - и плата сообщает, что он идёт и что кончился.

    Дожидаться конца обязательно: пока радио ходит по каналам, ответы платы
    (в том числе NTP соседних тестов) ждут, и следующий тест упал бы из-за
    этого, а не из-за себя.
    """
    code, _ = board.post_raw("/wifi", {"action": "scan"})
    assert code == 202

    for _ in range(30):
        if not board.get_json("/wifi")["scanning"]:
            break
        time.sleep(0.5)
    else:
        raise AssertionError("скан не закончился за 15 с")
