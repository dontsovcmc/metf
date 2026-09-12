# Testing

Three layers, one directory per suite. Each PlatformIO env picks its own suite with `test_filter`, so a host suite is never uploaded and a board suite is never compiled without Arduino.

```bash
# Host: NTP packet rules (test/test_ntp_packet), no board needed
pio test -e native
pio test -e native -f test_ntp_packet            # a single suite

# On-device Unity tests (test/test_board): hex helpers from lib/utils
pio test -e nodemcuv2

# Live board from the outside: pytest, standard library only (test/board)
pytest test/board --metf-host <ip> -v
pytest test/board --metf-host <ip> -k test_server_answers   # a single test
```

- `test/test_board` asserts NodeMCU pin constants (`D0`, `D5`, `LED_BUILTIN == 2`), so it is a NodeMCU suite - it does not compile for `esp32-c6-super-mini`.
- Without `--metf-host` the pytest suite skips: a test that cannot reach a board must say so, not time out.
- Host tests check the protocol rules, `test/board` checks that the board really listens, answers and serves the assigned time. One without the other is not enough: a packet can be built right and never sent, or sent from the wrong port.
- Code that should be host-tested must be header-only pure C++ with no `Arduino.h` (like `src/ntp_packet.h`); the `native` env only adds `-Isrc`.
- The client in `test/board/conftest.py` (`Board`: `get`, `get_json`, `post`) is the only Python client in this repository. The `ESPTestFramework` library shown in `README.md` lives elsewhere.
