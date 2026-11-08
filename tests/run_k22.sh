#!/usr/bin/env bash
# k22: aur_kernel path-derivation unit test — see
# tests/test_k22_aur_kernel.cpp. The AUR build-tree path
# (detail::aur_pkgbuilds_path) is the single derivation point for where
# kernel AUR package build trees live; it combines the user-selectable
# build directory (utils::build_repo_path, QSettings "buildDir", default
# ~/.cache/kernel-manager/pkgbuilds) with the aur_pkgbuilds/<package>
# suffix. A regression here silently sends makepkg into the wrong
# directory.
#
# Coverage: default build dir, no-package-name, with-package-name,
# custom build dir (re-read on demand, no cache), tilde expansion,
# trailing-separator stripping, empty set_build_dir no-op.
#
# Recipe: standalone g++ against the real aur_kernel.cpp + utils.cpp
# (run_k7.sh precedent), linking fmt + glib + Qt6 + libalpm. utils.hpp
# includes <QWidget>, so the full Qt include path is required. The
# harness sets XDG_CONFIG_HOME + XDG_CACHE_HOME to fresh mktemp
# sandboxes for the whole run, so the QSettings file and the default
# buildDir are isolated from the user's real state; the sandboxes are
# removed by the trap below on every exit path. The test's main()
# re-pins both variables via setenv before any utils:: call, so the
# binary is also safe to run standalone (the k15 D4 seam).
#
# Gates:
#   1. the test compiles with 0 source warnings (the lto-wrapper
#      serial-compilation note is tooling, not code — same exclusion as k12)
#   2. the test binary exits 0 (all A–G checks pass)
#   3. the user's real settings file is byte-identical before/after
#      (nothing leaks into the real config — the k15 real-config gate)
#   4. no k22 sandbox leaked into /tmp
set -euo pipefail
cd "$(dirname "$0")/.."

ROOT="$(pwd)"
BUILD="$ROOT/${KM_BUILD_DIR:-build}"
source "$ROOT/tests/common.sh"

# --- fmt artifacts (run_k12/k13 pattern) ---------------------------------
# resolve_* handles the Debug-postfix lib name (libfmtd.a), the CPM source
# cache, and main-worktree bootstrap.
FMT_INC="$(resolve_fmt_include)" || exit 1
FMT_LIB="$(resolve_fmt_lib)" || exit 1

# --- runner sandbox + the system-state gates ----------------------------
SANDBOX="$(mktemp -d /tmp/km22-sandbox.XXXXXX)"
trap 'rm -rf "$SANDBOX"' EXIT

# Isolate QSettings + the default buildDir cache in the sandbox.
export XDG_CONFIG_HOME="$SANDBOX/config"
export XDG_CACHE_HOME="$SANDBOX/cache"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"

# /etc/pacman.conf must come out byte-identical (the k15 real-config gate).
if [[ -f /etc/pacman.conf ]]; then
    md5sum /etc/pacman.conf > "$SANDBOX/pacman-conf.md5"
fi

OUT="/tmp/km-test-k22"
LOG="$SANDBOX/build.log"
: > "$LOG"

/usr/bin/c++ \
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
    $KM_CXX_FLAGS $KM_LINK_FLAGS \
    tests/test_k22_aur_kernel.cpp \
    src/aur_kernel.cpp \
    src/utils.cpp \
    "$FMT_LIB" \
    -lgcc_s -lutil -lrt -lpthread -lm -ldl -lc \
    /usr/lib/libglib-2.0.so \
    /usr/lib/libQt6Widgets.so \
    /usr/lib/libQt6Gui.so \
    /usr/lib/libQt6Core.so \
    -lalpm \
    -o "$OUT" 2>> "$LOG" \
    || { echo "error: k22 compile failed:" >&2; tail -n 40 "$LOG" >&2; exit 1; }

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

# --- post-run system-state gates ---------------------------------------
# /etc/pacman.conf byte-identical (the k15 real-config gate).
if [[ -f "$SANDBOX/pacman-conf.md5" ]]; then
    if ! md5sum -c "$SANDBOX/pacman-conf.md5" >/dev/null 2>&1; then
        echo "error: /etc/pacman.conf was modified during k22 (real-config gate)" >&2
        exit 1
    fi
fi

# No k22 sandbox leaked into /tmp (the binary removes its own; the
# runner sandbox is removed by the trap).
if ls -d /tmp/km-test-k22-* 1>/dev/null 2>&1; then
    echo "error: k22 sandbox leak:" >&2
    ls -d /tmp/km-test-k22-* >&2
    exit 1
fi

rm -f "$OUT" "$LOG"
echo "K22: OK"
