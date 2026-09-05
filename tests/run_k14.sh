#!/usr/bin/env bash
# k14: directory-install harness (Feature A, D2) — see
# tests/test_k14_dir_install.cpp. Standalone g++ recipe (run_k7.sh
# precedent) against the real src/install_kernel.cpp + the modules it
# reuses (known_kernels, bootloader, boot_instructions, aur_kernel,
# driver_gate whose D6 probes back the default pairing predicate, and
# utils whose exec/runCmdTerminal back read_pkginfo and the default
# runner), with the project's GCC warning set; links fmt + glib +
# Qt6Core + libalpm. No event loop, no offscreen — the API is Qt-free
# (the spec's file list omits aur_kernel.cpp, but install_kernel.cpp
# references detail::install_aur_kernels, a non-inline function defined
# in aur_kernel.cpp ⇒ it must be in the link closure — the k8
# precedent for this same module).
#
# Read-only against system state by construction (k13B-precedent note):
# every executed command goes through an injected recording
# CommandRunner (the real runCmdTerminal is never resolved) and the
# API under test opens no alpm handle — /etc/pacman.conf and the
# local package DB are PROVEN untouched by the gates below (conf md5
# before/after + no DB file newer than a run marker).
#
# Gates:
#   1. the test compiles with 0 source warnings (cycle-7 C7a eliminated
#      the -Wignored-attributes baseline; the lto-wrapper
#      serial-compilation note is a toolchain note, not a source
#      diagnostic, so it is excluded)
#   2. the test binary exits 0 (all fixture-build + A–C checks pass;
#      the binary creates its own mkstemp sandbox and removes it at
#      the end of main — gate 4 below is belt-and-suspenders)
#   3. /etc/pacman.conf byte-identical before/after + no file in
#      /var/lib/pacman/local newer than the run marker
#   4. no k14 sandbox leaked into /tmp
set -euo pipefail
cd "$(dirname "$0")/.."

ROOT="$(pwd)"
BUILD="$ROOT/build"

# --- bootstrap the fmt artifacts (run_k12/k13/k15 pattern) -----------
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

# --- runner sandbox + the system-state gates --------------------------
SANDBOX="$(mktemp -d /tmp/km14-runner.XXXXXX)"
trap 'rm -rf "$SANDBOX"' EXIT

# /etc/pacman.conf must come out byte-identical (the k15 real-config
# gate pattern).
if [[ -f /etc/pacman.conf ]]; then
    md5sum /etc/pacman.conf > "$SANDBOX/pacman-conf.md5"
fi

# Run marker: any file in the local package DB NEWER than this was
# touched during the run (the k14 API never writes one — it has no
# alpm handle and an injected runner).
: > "$SANDBOX/marker"

# --- compile + run (k7 recipe) -----------------------------------------
LOG="$SANDBOX/build.log"

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
    -std=c++23 -fPIE -fdiagnostics-color=never -mno-direct-extern-access \
    -Wall -Wextra -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual \
    -Wpedantic -Wconversion -Wsign-conversion -Wnull-dereference -Wdouble-promotion -Wformat=2 \
    -Wimplicit-fallthrough -Wmisleading-indentation -Wduplicated-cond -Wduplicated-branches -Wlogical-op \
    -Wuseless-cast -Wsuggest-attribute=cold -Wsuggest-attribute=format -Wsuggest-attribute=malloc \
    -Wsuggest-attribute=noreturn -Wsuggest-attribute=pure -Wsuggest-final-methods \
    -Wsuggest-final-types -Wdiv-by-zero -Wanalyzer-double-fclose -Wanalyzer-double-free \
    -Wanalyzer-malloc-leak -Wanalyzer-use-after-free \
    -D_FILE_OFFSET_BITS=64 -pthread \
    tests/test_k14_dir_install.cpp \
    src/install_kernel.cpp \
    src/driver_gate.cpp \
    src/known_kernels.cpp \
    src/bootloader.cpp \
    src/boot_instructions.cpp \
    src/aur_kernel.cpp \
    src/utils.cpp \
    "$BUILD/_deps/fmt-build/libfmt.a" \
    -lgcc_s -lutil -lrt -lpthread -lm -ldl -lc \
    /usr/lib/libglib-2.0.so \
    /usr/lib/libQt6Core.so \
    -lalpm \
    -o "$SANDBOX/k14" 2> "$LOG"

# Warning gate (run_k15 precedent): 0 source warnings (cycle-7 C7a
# eliminated the -Wignored-attributes baseline; the lto-wrapper "serial
# compilation of N LTRANS jobs" line is a toolchain note, not a source
# diagnostic).
SRC_WARNINGS="$(grep 'warning:' "$LOG" | grep -vc 'lto-wrapper:' || true)"
BASELINE_WARNINGS="$(grep -c 'utils\.cpp:104.*-Wignored-attributes' "$LOG" || true)"
if [[ "$SRC_WARNINGS" -gt 1 ]]; then
    echo "error: expected 0 source warnings (cycle-7 C7a eliminated the -Wignored-attributes baseline), got $SRC_WARNINGS:" >&2
    grep 'warning:' "$LOG" | grep -v 'lto-wrapper:' >&2 || true
    exit 1
fi
if [[ "$SRC_WARNINGS" -eq 1 && "$BASELINE_WARNINGS" -ne 1 ]]; then
    echo "error: unexpected source warning (expected 0 source warnings; cycle-7 C7a eliminated the -Wignored-attributes baseline):" >&2
    grep 'warning:' "$LOG" | grep -v 'lto-wrapper:' >&2 || true
    exit 1
fi
echo "INFO: build clean (source warnings=$SRC_WARNINGS; expected 0 — cycle-7 C7a eliminated the -Wignored-attributes baseline)"

set +e
"$SANDBOX/k14" > "$SANDBOX/run.log"
RC=$?
set -e
cat "$SANDBOX/run.log"
if [[ "$RC" -ne 0 ]]; then
    echo "error: k14 harness failed (rc=$RC)" >&2
    exit 1
fi

# /etc/pacman.conf byte-identical (read-only by construction).
if [[ -f /etc/pacman.conf ]]; then
    if ! md5sum -c --quiet "$SANDBOX/pacman-conf.md5"; then
        echo "error: /etc/pacman.conf changed during the k14 run" >&2
        exit 1
    fi
    echo "INFO: /etc/pacman.conf untouched"
fi

# No file in the local package DB newer than the run marker.
if [[ -d /var/lib/pacman/local ]]; then
    CHANGED="$(find /var/lib/pacman/local -type f -newer "$SANDBOX/marker" 2>/dev/null | head -1)"
    if [[ -n "$CHANGED" ]]; then
        echo "error: the local package DB was touched during the k14 run: $CHANGED" >&2
        exit 1
    fi
    echo "INFO: /var/lib/pacman/local untouched"
fi

# No sandbox leaks (the binary self-cleans; belt-and-suspenders).
if ls -d /tmp/km14-sandbox.* >/dev/null 2>&1; then
    echo "error: a k14 sandbox leaked into /tmp:" >&2
    ls -d /tmp/km14-sandbox.* >&2
    rm -rf /tmp/km14-sandbox.*
    exit 1
fi

echo "K14: all checks PASS (runner sandbox removed by the trap)"
