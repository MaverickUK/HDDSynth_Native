#!/bin/sh
# Build and run the host-side unit tests (macOS: clang++, Linux: g++).
# Usage: test/run_native_tests.sh [path/to/HDDSynth_Samples]
set -e
cd "$(dirname "$0")/.."
CXX="${CXX:-c++}"
mkdir -p .pio/native-tests
"$CXX" -std=c++17 -O2 -Wall -Wextra -pthread -fsanitize=address,undefined \
    -Isrc test/native/test_main.cpp src/wav.cpp src/mixer.cpp src/powerlog_format.cpp src/persist_format.cpp src/beep.cpp src/levels_format.cpp src/canvas.cpp src/oled_screens.cpp src/link_parser.cpp src/sentence.cpp src/host_protocol.cpp \
    -o .pio/native-tests/run_tests
.pio/native-tests/run_tests "$@"
