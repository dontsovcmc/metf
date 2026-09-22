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
- `test/board/test_pulse.py` is where the non-blocking `/pulse` is proven: it pings the board throughout a 2-second pulse and fails if any of those pings waits. That property lives only on the board - no host test can see it.
- Code that should be host-tested must be header-only pure C++ with no `Arduino.h` (like `src/ntp_packet.h`); the `native` env only adds `-Isrc`.
- The client in `test/board/conftest.py` (`Board`: `get`, `get_json`, `post`) is the only Python client in this repository. The `ESPTestFramework` library shown in `README.md` lives elsewhere.

## Static analysis and warnings

Three tools, because no single one sees all of the code:

```bash
pio check -e esp32-c6-super-mini --severity=medium   # cppcheck: src/, lib/, host tests
pio check -e nodemcuv2 --severity=medium
scripts/tidy.sh                                        # clang-tidy: host-buildable code
pio run -e esp32-c6-super-mini && pio run -e nodemcuv2 # -Wall -Wextra on src/
```

- **cppcheck** (`pio check`) covers everything, Arduino included, because it does not need to parse every header. Settings are in `[env]` of `platformio.ini`; `check_skip_packages = yes` keeps the core and libraries out.
- **clang-tidy** (`scripts/tidy.sh`) covers only what builds on the host: the pure-C++ classes in `src/` (`WifiPolicy`, `Blinker`, `ntp_packet.h`) and the host test suites. It is not run on Arduino code on purpose: clang cannot parse the ESP toolchain's headers (`FreeRTOS.h`, the riscv32 `stddef.h` give parse errors), and every finding after a parse error is noise - "variable is not initialized" on a `const` with an initializer, "can be made static" on a method that uses members. The check list is `.clang-tidy`, with the reason for every disabled check beside it.
- The clang-tidy bundled with PlatformIO has neither its own builtin headers nor the macOS SDK; the script passes both from the Command Line Tools. Without them it reports the same noise on host code too.
- **`build_src_flags = -Wall -Wextra`** applies to `src/` only, so library warnings do not bury ours.
- The target for new code is zero warnings and zero `medium`/`high` findings. Older code is fixed when it is touched.
- `.clang-format` is applied to new and moved files, not to the whole tree at once - a mass reformat would bury every real change in the history.
