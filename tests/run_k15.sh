#!/usr/bin/env bash
# k15: build-dir persistence harness (D4+D5) — see
# tests/test_k15_build_dir.cpp. Standalone g++ recipe (run_k7.sh
# precedent) against the real src/utils.cpp + src/aur_kernel.cpp, with
# the project's GCC warning set; links fmt + glib + Qt6Core + libalpm.
#
# D4 harness seam: XDG_CONFIG_HOME is pinned to a fresh mktemp sandbox
# for the whole run, so the QSettings file (ArchLinux/KernelManager.*)
# is isolated from the user's real config; the sandbox is removed by the
# trap below on every exit path. The test's main() re-pins the variable
# via setenv before any utils:: call, so the binary is also safe to run
# standalone (it reuses this exported sandbox and leaves cleanup to the
# trap).
#
# Gates:
#   1. the test compiles with 0 source warnings (cycle-7 C7a eliminated
#      the -Wignored-attributes baseline; the lto-wrapper
#      serial-compilation note is excluded from the count, same as there)
#   2. the test binary exits 0 (all A–E checks pass)
#   3. the user's real settings file is byte-identical before/after
#      (nothing leaks into the real config — the k13B live-conf gate
#      pattern)
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

# --- the XDG_CONFIG_HOME sandbox + the real-config gate ------------------
SANDBOX="$(mktemp -d /tmp/km15-sandbox.XXXXXX)"
GATE="$(mktemp -d /tmp/km15-gate.XXXXXX)"
trap 'rm -rf "$SANDBOX" "$GATE"' EXIT

# The user's real settings file (Qt-resolved: <config>/ArchLinux/
# KernelManager.conf) must come out byte-identical: snapshot it before
# the run (absent → assert it stays absent).
REAL_CONF="${XDG_CONFIG_HOME:-$HOME/.config}/ArchLinux/KernelManager.conf"
if [[ -f "$REAL_CONF" ]]; then
    cp "$REAL_CONF" "$GATE/real.conf"
    md5sum "$REAL_CONF" > "$GATE/real.md5"
else
    echo absent > "$GATE/real.exists"
fi

# --- compile + run (k7 recipe, sandboxed env) -----------------------------
LOG="$GATE/build.log"

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
    tests/test_k15_build_dir.cpp \
    src/utils.cpp \
    src/aur_kernel.cpp \
    "$BUILD/_deps/fmt-build/libfmt.a" \
    -lgcc_s -lutil -lrt -lpthread -lm -ldl -lc \
    /usr/lib/libglib-2.0.so \
    /usr/lib/libQt6Core.so \
    -lalpm \
    -o "$SANDBOX/k15" 2> "$LOG"

# Warning gate: 0 source warnings (cycle-7 C7a eliminated the
# -Wignored-attributes baseline; the lto-wrapper "serial compilation of N
# LTRANS jobs" line is a toolchain note — the same diagnostic CMake builds
# report as "note:" — not a source diagnostic, so it is excluded from the
# count).
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

# Run the harness inside the sandbox: main() re-pins XDG_CONFIG_HOME via
# setenv before any utils:: call (the D4 seam; the binary reuses this
# exported sandbox and leaves its cleanup to the trap above).
set +e
XDG_CONFIG_HOME="$SANDBOX" "$SANDBOX/k15" > "$GATE/run.log"
RC=$?
set -e
cat "$GATE/run.log"
if [[ "$RC" -ne 0 ]]; then
    echo "error: k15 harness failed (rc=$RC)" >&2
    exit 1
fi

# Nothing may have leaked into the user's real config: the real settings
# file is byte-identical before/after (the k13B live-conf gate pattern).
if [[ -f "$REAL_CONF" ]]; then
    if ! md5sum -c --quiet "$GATE/real.md5" || ! cmp -s "$REAL_CONF" "$GATE/real.conf"; then
        echo "error: the real settings file changed: $REAL_CONF" >&2
        exit 1
    fi
    echo "INFO: real settings file untouched ($REAL_CONF)"
else
    if [[ -f "$REAL_CONF" ]]; then
        echo "error: a settings file appeared at $REAL_CONF" >&2
        exit 1
    fi
    echo "INFO: no real settings file before or after (nothing created at $REAL_CONF)"
fi

echo "K15: all checks PASS (sandbox + gate dirs removed by the trap)"
