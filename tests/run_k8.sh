#!/usr/bin/env bash
# Standalone K8 unit test (no CTest infra in this project; follows the
# "standalone g++ on the real sources" precedent of tests/run_k6.sh).
# Compiles the real install_kernel module plus the modules it reuses
# (known_kernels, bootloader, boot_instructions, aur_kernel, and utils
# whose runCmdTerminal/exec back the default runner and the paru
# probe) with the project's GCC warning set, and runs the plan /
# execute / boot-instructions assertions. Every executed command goes
# through an injected recording CommandRunner, so no real terminal,
# pkexec, pacman, paru or network is touched (environment-tolerant).
# The pre-existing utils.cpp:103 -Wignored-attributes warning is
# expected (run_k7.sh precedent); no other warning may appear.
set -euo pipefail
cd "$(dirname "$0")/.."

ROOT="$(pwd)"
BUILD="$ROOT/build"

OUT="/tmp/km-test-k8"

/usr/bin/c++ \
    -DKM_HELPER_DIR=\"/usr/lib/kernel-manager\" \
    -DKM_IGNORE_REPO=\"\" \
    -DQT_CORE_LIB \
    -I"$ROOT/src" \
    -I"$BUILD/_deps/fmt-src/include" \
    -isystem /usr/include/qt6/QtCore \
    -isystem /usr/include/qt6 \
    -isystem /usr/lib/qt6/mkspecs/linux-g++ \
    -isystem /usr/include/glib-2.0 \
    -isystem /usr/lib/glib-2.0/include \
    -isystem /usr/include/sysprof-6 \
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
    tests/test_k8_install_kernel.cpp \
    src/install_kernel.cpp \
    src/known_kernels.cpp \
    src/bootloader.cpp \
    src/boot_instructions.cpp \
    src/aur_kernel.cpp \
    src/utils.cpp \
    "$BUILD/_deps/fmt-build/libfmt.a" \
    -lgcc_s -lutil -lrt -lpthread -lm -ldl -lc \
    /usr/lib/libglib-2.0.so \
    /usr/lib/libQt6Core.so \
    -o "$OUT"

"$OUT"
