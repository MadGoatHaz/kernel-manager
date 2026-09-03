#!/usr/bin/env bash
# Standalone K18 unit test (no CTest infra in this project; follows the
# "standalone g++ on the real sources" precedent of tests/run_k5.sh).
# The distro module is pure C++ (no Qt, no external deps), so this
# compiles the real source with the project's GCC warning set and runs
# the detection assertions against fake os-release probes only.
set -euo pipefail
cd "$(dirname "$0")/.."

OUT="/tmp/km-test-k18"

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
    tests/test_k18_distro.cpp \
    src/distro.cpp \
    -o "$OUT"

"$OUT"
