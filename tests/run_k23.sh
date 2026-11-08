#!/usr/bin/env bash
# k23: PKGBUILD custom-name rewrite unit test (T-044) — see
# tests/test_k23_pkgbuild_custom_name.cpp. utils::apply_pkgbuild_custom_name
# is the generic function that lets a user build a custom-named kernel
# package: it rewrites the first top-level pkgbase= line in a PKGBUILD
# with a user-supplied name, expanding a "$pkgbase" / "${pkgbase}"
# placeholder against the file's original pkgbase value. The function is
# pure (takes content, returns a result struct) — no filesystem,
# QSettings, or alpm state is involved — so the test is entirely
# in-memory and needs no sandbox.
#
# Coverage: bare rename, $pkgbase / ${pkgbase} / mixed placeholders,
# idempotency (single and double-stacked), no-op (empty / identity
# names), injection guards (semicolon / backtick / double-quote),
# structural rejections (missing pkgver / pkgbase), pkgver() function
# form, nested pkgbase left untouched.
#
# Recipe: standalone g++ against the real utils.cpp (run_k7.sh
# precedent), linking fmt + glib + Qt6 + libalpm. utils.hpp includes
# <QWidget>, so the full Qt include path (QtWidgets/QtGui/QtCore) is
# required to compile it, even though the function under test is Qt-free.
#
# Gates:
#   1. the test compiles with 0 source warnings (the lto-wrapper
#      serial-compilation note is tooling, not code — same exclusion as k12)
#   2. the test binary exits 0 (all A–P checks pass)
set -euo pipefail
cd "$(dirname "$0")/.."

ROOT="$(pwd)"
BUILD="$ROOT/build"

# --- bootstrap the fmt artifacts (run_k12/k13 pattern) -------------------
# The standalone recipe needs the fmt include dir + static lib from the
# project's CMake build dir (the CMake configure is their only producer;
# they are version-pinned). If this worktree has no build/ of its own,
# copy them from the main worktree's build.
for f in \
    "$BUILD/_deps/fmt-src/include" \
    "$BUILD/_deps/fmt-build/libfmt.a"
do
    if [[ ! -e "$f" ]]; then
        MAIN_ROOT="$(git -C "$ROOT" worktree list --porcelain | awk '/^worktree /{print $2; exit}')"
        REL="${f#"$BUILD"/}"
        if [[ -n "${MAIN_ROOT}" && -e "${MAIN_ROOT}/build/${REL}" ]]; then
            mkdir -p "$(dirname "$f")"
            cp -r "${MAIN_ROOT}/build/${REL}" "$f"
        else
            echo "error: CMake artifact $f not found in $BUILD nor in the main worktree's build (run the CMake build first)" >&2
            exit 1
        fi
    fi
done

OUT="/tmp/km-test-k23"
LOG="$OUT-build.log"
: > "$LOG"

/usr/bin/c++ \
    -DKM_HELPER_DIR=\"/usr/lib/kernel-manager\" \
    -DKM_IGNORE_REPO=\"\" \
    -DQT_CORE_LIB -DQT_GUI_LIB -DQT_WIDGETS_LIB \
    -I"$ROOT/src" \
    -I"$BUILD/_deps/fmt-src/include" \
    -isystem /usr/include/qt6/QtWidgets \
    -isystem /usr/include/qt6/QtGui \
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
    tests/test_k23_pkgbuild_custom_name.cpp \
    src/utils.cpp \
    "$BUILD/_deps/fmt-build/libfmt.a" \
    -lgcc_s -lutil -lrt -lpthread -lm -ldl -lc \
    /usr/lib/libglib-2.0.so \
    /usr/lib/libQt6Widgets.so \
    /usr/lib/libQt6Gui.so \
    /usr/lib/libQt6Core.so \
    -lalpm \
    -o "$OUT" 2>> "$LOG" \
    || { echo "error: k23 compile failed:" >&2; tail -n 40 "$LOG" >&2; exit 1; }

# Warning gate: 0 source warnings (the lto-wrapper serial-compilation
# note is tooling, not code — same exclusion as k12).
SRC_WARNINGS="$(grep 'warning:' "$LOG" | grep -vc 'lto-wrapper:' || true)"
if [[ "$SRC_WARNINGS" -gt 0 ]]; then
    echo "error: expected 0 source warnings, got $SRC_WARNINGS:" >&2
    grep 'warning:' "$LOG" | grep -v 'lto-wrapper:' >&2 || true
    exit 1
fi
echo "INFO: build clean (source warnings=0)"

"$OUT"

rm -f "$OUT" "$LOG"
echo "K23: OK"
