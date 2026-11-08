#!/usr/bin/env bash
# Offscreen UI driver for the chunk-2 source dropdown (no CTest infra in
# this project; follows tests/run_chunk1.sh: compiles the REAL sources —
# conf-window.cpp is included by the test so the window can be driven
# directly — with the project's own include/define set, then runs the
# dropdown/auto-populate assertions offscreen).
set -euo pipefail
cd "$(dirname "$0")/.."

ROOT="$(pwd)"
BUILD="$ROOT/${KM_BUILD_DIR:-build}"
source "$ROOT/tests/common.sh"

FMT_INC="$(resolve_fmt_include)" || exit 1
FMT_LIB="$(resolve_fmt_lib)" || exit 1
FROZEN_INC="$(resolve_frozen_include)" || exit 1

# The autogen hash dir differs per build tree; locate the page/window mocs
# (the page classes are header-only Q_OBJECTs — their vtables live in the
# generated mocs, which the ConfWindow ctor needs at link time).
AUTODIR="$(resolve_autogen_dir)" || exit 1
MOC_CONF_WINDOW="$(ls "$AUTODIR"/*/moc_conf-window.cpp 2>/dev/null | head -n1 || true)"
MOC_CONF_OPTIONS_PAGE="$(ls "$AUTODIR"/*/moc_conf-options-page.cpp 2>/dev/null | head -n1 || true)"
MOC_CONF_PATCHES_PAGE="$(ls "$AUTODIR"/*/moc_conf-patches-page.cpp 2>/dev/null | head -n1 || true)"
if [[ -z "${MOC_CONF_WINDOW}" || -z "${MOC_CONF_OPTIONS_PAGE}" || -z "${MOC_CONF_PATCHES_PAGE}" ]]; then
    echo "error: ConfWindow/ConfOptionsPage/ConfPatchesPage mocs not found under $AUTODIR (run the CMake build first)" >&2
    exit 1
fi

OUT="/tmp/km-test-chunk2-ui"

${KM_CXX} \
    -DKM_HELPER_DIR=\"/usr/lib/kernel-manager\" \
    -DKM_IGNORE_REPO=\"\" \
    -DQT_CONCURRENT_LIB -DQT_CORE_LIB -DQT_DISABLE_DEPRECATED_BEFORE=0x050F00 -DQT_GUI_LIB -DQT_NO_DEBUG -DQT_WIDGETS_LIB \
    -I"$AUTODIR/include" \
    -I"$ROOT/src" \
    -I"$BUILD" \
    -I"$FMT_INC" \
    -I"$FROZEN_INC" \
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
    -O3 ${KM_LTO_FLAGS} \
    -std=c++23 -fPIE -fdiagnostics-color=always \
    -Wall -Wextra -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual \
    -Wpedantic -Wconversion -Wsign-conversion -Wnull-dereference -Wdouble-promotion -Wformat=2 \
    -Wimplicit-fallthrough -Wmisleading-indentation \
    ${KM_GCC_WARNING_FLAGS} \
    \
    -Wdiv-by-zero ${KM_ANALYZER_FLAGS} \
    -D_FILE_OFFSET_BITS=64 -pthread \
    $KM_CXX_FLAGS $KM_LINK_FLAGS \
    tests/test_chunk2_conf_window.cpp \
    src/install_kernel.cpp \
    src/driver_gate.cpp \
    src/utils.cpp \
    src/config-options.cpp \
    src/known_kernels.cpp \
    "$MOC_CONF_WINDOW" \
    "$MOC_CONF_OPTIONS_PAGE" \
    "$MOC_CONF_PATCHES_PAGE" \
    "$FMT_LIB" \
    "$BUILD/libconfig-option-lib-cxxbridge.a" \
    "$BUILD/libconfig_option_lib.a" \
    -lgcc_s -lutil -lrt -lpthread -lm -ldl -lc \
    /usr/lib/libglib-2.0.so \
    /usr/lib/libQt6Widgets.so \
    /usr/lib/libQt6Gui.so \
    /usr/lib/libQt6Core.so \
    -o "$OUT"

QT_QPA_PLATFORM=offscreen "$OUT"
