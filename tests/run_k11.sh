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
BUILD="$ROOT/${KM_BUILD_DIR:-build}"
source "$ROOT/tests/common.sh"

# The harness needs only the fmt artifacts (header + static lib) from the
# project's CPM build dir. resolve_* handles the Debug-postfix lib name
# (libfmtd.a), the CPM source cache, and main-worktree bootstrap.
FMT_INC="$(resolve_fmt_include)" || exit 1
FMT_LIB="$(resolve_fmt_lib)" || exit 1

OUT="/tmp/km-test-k11"

${KM_CXX} \
    -DKM_HELPER_DIR=\"/usr/lib/kernel-manager\" \
    -DKM_IGNORE_REPO=\"\" \
    -DQT_CORE_LIB -DQT_GUI_LIB -DQT_WIDGETS_LIB \
    -I"$ROOT/src" \
    -I"$FMT_INC" \
    -isystem /usr/include/qt6/QtWidgets \
    -isystem /usr/include/qt6/QtGui \
    -isystem /usr/include/qt6/QtCore \
    -isystem /usr/include/qt6 \
    -isystem /usr/lib/qt6/mkspecs/linux-g++ \
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
    tests/test_k11_tree_rows.cpp \
    src/kernel.cpp \
    src/known_kernels.cpp \
    src/alpm_utils.cpp \
    src/utils.cpp \
    "$FMT_LIB" \
    -lgcc_s -lutil -lrt -lpthread -lm -ldl -lc \
    /usr/lib/libglib-2.0.so \
    /usr/lib/libQt6Widgets.so \
    /usr/lib/libQt6Gui.so \
    /usr/lib/libQt6Core.so \
    -lalpm \
    -o "$OUT"

"$OUT"
