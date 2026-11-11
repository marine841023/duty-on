#!/bin/sh
# Native Linux regression tests, without graphics/SDK/audio dependencies.
# Usage: ./run-device-tests.sh /path/to/nlohmann-json/include
set -eu
src=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build=$(mktemp -d /tmp/dutyon-test-build-XXXXXX)
trap 'rm -rf "$build"' EXIT HUP INT TERM
json_include=${1:-/usr/include}
"${CXX:-g++}" -std=c++17 -O0 -Wall -Wextra -pthread \
    -I"$src/src" -I"$json_include" \
    "$src/tests/device_tests.cpp" "$src/src/config/user_config.cpp" \
    "$src/src/audio/sound_player.cpp" -o "$build/device-tests"
timeout -k 2 30 "$build/device-tests"
