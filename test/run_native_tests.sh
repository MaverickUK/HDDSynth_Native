#!/bin/sh
# Build and run the host-side unit tests (macOS: clang++, Linux: g++).
# Usage: test/run_native_tests.sh [path/to/HDDSynth_Samples]
set -e
cd "$(dirname "$0")/.."
CXX="${CXX:-c++}"
mkdir -p .pio/native-tests
"$CXX" -std=c++17 -O2 -Wall -Wextra -pthread -fsanitize=address,undefined \
    -Isrc test/native/test_main.cpp src/wav.cpp src/mixer.cpp src/powerlog_format.cpp src/persist_format.cpp src/beep.cpp src/levels_format.cpp src/canvas.cpp src/oled_screens.cpp src/link_parser.cpp src/sentence.cpp src/host_protocol.cpp src/drive_engine.cpp src/drive_params.cpp src/drive_timing.cpp \
    -o .pio/native-tests/run_tests
.pio/native-tests/run_tests "$@"

# The modelled drive against the browser prototype (tools/drive-lab), if node is installed.
"$CXX" -std=c++17 -O2 -Wall -Wextra -fsanitize=address,undefined \
    -Isrc test/tools/render_drive.cpp src/drive_engine.cpp src/drive_params.cpp src/drive_timing.cpp src/sentence.cpp \
    -o .pio/native-tests/render_drive
if command -v node >/dev/null 2>&1; then
    node tools/drive-lab/compare.js .pio/native-tests/render_drive
else
    echo "node not installed: skipping the comparison with the drive prototype"
fi
