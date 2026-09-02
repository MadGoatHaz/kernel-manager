#!/usr/bin/env bash
# Standalone K11 (E17) unit test: the Kernel::get_kernels curated info-row
# harness (no CTest infra in this project; follows the "standalone g++ on the
# real sources" precedent of tests/run_k7.sh). Compiles the real kernel.cpp
# (plus known_kernels.cpp, alpm_utils.cpp and utils.cpp, whose parse_alpm /
# exec back the DB probes) with the project's GCC warning set, links fmt +
# libalpm, and runs the row assertions (no Qt event loop needed; Qt6Core is
# linked only because utils.hpp includes <QString>). Environment-tolerant:
# every expected set is derived from the live local/sync DBs and the curated
# table — no hard-coded row counts.
# 0 source warnings (cycle-7 C7a eliminated the -Wignored-attributes
# baseline); no other warning may appear.
set -euo pipefail
cd "$(dirname "$0")/.."

ROOT="$(pwd)"
BUILD="$ROOT/build"

# The harness needs only the fmt artifacts (header + static lib) from the
# project's CPM build dir. If this worktree has no build/ of its own (or the
# fmt pieces are absent), copy them from the main worktree's build — the
# CMake configure is their only producer and they are version-pinned by CPM.
if [[ ! -d "$BUILD/_deps/fmt-src/include" || ! -f "$BUILD/_deps/fmt-build/libfmt.a" ]]; then
    MAIN_ROOT="$(git -C "$ROOT" worktree list --porcelain | awk '/^worktree /{print $2; exit}')"
    if [[ -n "$MAIN_ROOT" && -d "$MAIN_ROOT/build/_deps/fmt-src/include" && -f "$MAIN_ROOT/build/_deps/fmt-build/libfmt.a" ]]; then
        mkdir -p "$BUILD/_deps/fmt-build" "$BUILD/_deps/fmt-src"
        cp -r "$MAIN_ROOT/build/_deps/fmt-src/include" "$BUILD/_deps/fmt-src/"
        cp "$MAIN_ROOT/build/_deps/fmt-build/libfmt.a" "$BUILD/_deps/fmt-build/"
    else
        echo "error: fmt artifacts not found in $BUILD nor in the main worktree's build (run the CMake build first)" >&2
        exit 1
    fi
fi

OUT="/tmp/km-test-k11"

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
    tests/test_k11_tree_rows.cpp \
    src/kernel.cpp \
    src/known_kernels.cpp \
    src/alpm_utils.cpp \
    src/utils.cpp \
    "$BUILD/_deps/fmt-build/libfmt.a" \
    -lgcc_s -lutil -lrt -lpthread -lm -ldl -lc \
    /usr/lib/libglib-2.0.so \
    /usr/lib/libQt6Core.so \
    -lalpm \
    -o "$OUT"

"$OUT"
