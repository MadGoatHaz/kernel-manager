#!/usr/bin/env bash
# Standalone chunk-1 unit test (no CTest infra in this project; follows the
# established "standalone g++ on the real sources" precedent). Compiles the
# real conf-window.cpp / utils.cpp / config-options.cpp plus the generated
# moc of ConfWindow, with the project's own include/define set, then runs
# the path-logic assertions offscreen.
set -euo pipefail
cd "$(dirname "$0")/.."

ROOT="$(pwd)"
BUILD="$ROOT/build"

# The autogen hash dir differs per build tree; locate the ConfWindow moc.
MOC_CONF_WINDOW="$(ls "$BUILD/kernel-manager_autogen"/*/moc_conf-window.cpp 2>/dev/null | head -n1 || true)"
if [[ -z "${MOC_CONF_WINDOW}" ]]; then
    echo "error: moc_conf-window.cpp not found under $BUILD/kernel-manager_autogen (run the CMake build first)" >&2
    exit 1
fi

OUT="/tmp/km-test-chunk1"

/usr/bin/c++ \
    -DKM_HELPER_DIR=\"/usr/lib/kernel-manager\" \
    -DKM_IGNORE_REPO=\"\" \
    -DQT_CONCURRENT_LIB -DQT_CORE_LIB -DQT_DISABLE_DEPRECATED_BEFORE=0x050F00 -DQT_GUI_LIB -DQT_NO_DEBUG -DQT_WIDGETS_LIB \
    -I"$BUILD/kernel-manager_autogen/include" \
    -I"$ROOT/src" \
    -I"$BUILD" \
    -I"$BUILD/_deps/fmt-src/include" \
    -I"$BUILD/_deps/frozen-src/include" \
    -I"$BUILD/corrosion_generated/cxxbridge/config-option-lib-cxxbridge/include" \
    -isystem /usr/include/qt6/QtCore \
    -isystem /usr/include/qt6 \
    -isystem /usr/lib/qt6/mkspecs/linux-g++ \
    -isystem /usr/include/qt6/QtWidgets \
    -isystem /usr/include/qt6/QtGui \
    -isystem /usr/include/qt6/QtConcurrent \
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
    tests/test_chunk1_paths.cpp \
    src/utils.cpp \
    src/config-options.cpp \
    "$MOC_CONF_WINDOW" \
    "$BUILD/_deps/fmt-build/libfmt.a" \
    "$BUILD/libconfig-option-lib-cxxbridge.a" \
    "$BUILD/libconfig_option_lib.a" \
    -lgcc_s -lutil -lrt -lpthread -lm -ldl -lc \
    /usr/lib/libglib-2.0.so \
    /usr/lib/libQt6Widgets.so \
    /usr/lib/libQt6Gui.so \
    /usr/lib/libQt6Core.so \
    -o "$OUT"

QT_QPA_PLATFORM=offscreen "$OUT"
