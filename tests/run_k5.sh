#!/usr/bin/env bash
# Standalone K5 unit test (no CTest infra in this project; follows the
# "standalone g++ on the real sources" precedent of tests/run_chunk2.sh).
# The bootloader module is pure C++ (no Qt, no external deps), so this
# compiles the real source with the project's GCC warning set and runs
# the detection assertions against fake probes only.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT="/tmp/km-test-k5"

/usr/bin/c++ \
    -O3 -flto -fwhole-program -fuse-linker-plugin \
    -std=c++23 -fPIE -fdiagnostics-color=always -mno-direct-extern-access \
    -Wall -Wextra -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual \
    -Wpedantic -Wconversion -Wsign-conversion -Wnull-dereference -Wdouble-promotion -Wformat=2 \
    -Wimplicit-fallthrough -Wmisleading-indentation -Wduplicated-cond -Wduplicated-branches -Wlogical-op \
    -Wuseless-cast -Wsuggest-attribute=cold -Wsuggest-attribute=format -Wsuggest-attribute=malloc \
    -Wsuggest-attribute=noreturn -Wsuggest-attribute=pure -Wsuggest-final-methods \
    -Wsuggest-final-types -Wdiv-by-zero -Wanalyzer-double-fclose -Wanalyzer-double-free \
    -Wanalyzer-malloc-leak -Wanalyzer-use-after-free \
    -D_FILE_OFFSET_BITS=64 -pthread \
    -Isrc \
    tests/test_k5_bootloader.cpp \
    src/bootloader.cpp \
    -o "$OUT"

"$OUT"
