#!/bin/sh
# clang-tidy по коду, который собирается на хосте: чистый C++ из src/ и
# хостовые тесты. Правила - .clang-tidy в корне.
#
# Код с Arduino сюда не входит: clang не разбирает заголовки тулчейна ESP,
# и находки после ошибок разбора ложные. Его проверяют cppcheck (pio check)
# и -Wall -Wextra при сборке. См. docs/testing.md.
#
#   scripts/tidy.sh            все файлы
#   scripts/tidy.sh <file>...  только эти
set -eu
cd "$(dirname "$0")/.."

TIDY=$(command -v clang-tidy || echo "$HOME/.platformio/packages/tool-clangtidy/clang-tidy")
[ -x "$TIDY" ] || { echo "clang-tidy не найден: pio check -e native его поставит" >&2; exit 2; }

UNITY=.pio/libdeps/native/Unity/src
[ -d "$UNITY" ] || ~/.platformio/penv/bin/pio pkg install -e native >/dev/null

# clang-tidy из PlatformIO собран без своих заголовков и без SDK macOS:
# даём ему заголовки из Command Line Tools
EXTRA=""
if [ "$(uname)" = Darwin ]; then
    SDK=$(xcrun --show-sdk-path)
    RES=$(ls -d "$(xcrun --find clang | xargs dirname)"/../lib/clang/*/include | head -1)
    EXTRA="-isysroot $SDK -isystem $SDK/usr/include/c++/v1 -isystem $RES"
fi

FILES=${*:-"src/wifi_policy.cpp src/blinker.cpp src/wifi_reason.cpp test/test_ntp_packet/test_ntp_packet.cpp test/test_wifi_policy/test_main.cpp test/test_blinker/test_main.cpp test/test_wifi_reason/test_main.cpp"}

# shellcheck disable=SC2086
"$TIDY" --quiet --config-file=.clang-tidy $FILES -- \
    -std=c++17 -Isrc -I"$UNITY" -Itest/test_wifi_policy -Itest/test_blinker -Itest/test_wifi_reason $EXTRA 2>&1 \
    | grep -v "warnings generated" || true
