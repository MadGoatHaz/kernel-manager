#!/usr/bin/env bash
# k21: config-options (cxxbridge TOML) unit test (T-043) — see
# tests/test_k21_config_options.cpp. The config-options module is the
# largest untested logic module: it bridges the Qt UI and the Rust
# cxxbridge for TOML config parse/serialize. This harness compiles the
# REAL config-options.cpp against the real cxxbridge + fmt from the CMake
# build, then drives parse_from_file / write_config_file through a
# mkstemp sandbox (no system config touched).
#
# Coverage: fully-populated TOML parse, empty-document defaults, legacy
# document (missing keys), malformed TOML (nullopt), missing file
# (nullopt), write→parse round-trip (all fields), neutral field name in
# serialized output, all-default round-trip, TOML-special-character
# round-trip (quotes, backslashes).
#
# Recipe shape (run_k7.sh precedent): standalone g++ on the real sources
# with the project's GCC warning set, linking fmt + the cxxbridge Rust
# static libs + glib. No Qt needed (config-options is Qt-free).
#
# Gates:
#   1. the test compiles with 0 source warnings (the lto-wrapper
#      serial-compilation note is tooling, not code — same exclusion as k12)
#   2. the test binary exits 0 (all A–I checks pass)
#   3. the sandbox is removed (no /tmp leak)
set -euo pipefail
cd "$(dirname "$0")/.."

ROOT="$(pwd)"
BUILD="$ROOT/build"

# --- bootstrap the fmt + cxxbridge artifacts (run_k12 pattern) ---------
# The standalone recipe needs the fmt include + static lib and the
# cxxbridge static libs from the project's CMake build dir (the CMake
# configure is their only producer; they are version-pinned). If this
# worktree has no build/ of its own (or any piece is absent), copy them
# from the main worktree's build.
for f in \
    "$BUILD/_deps/fmt-src/include" \
    "$BUILD/_deps/fmt-build/libfmt.a" \
    "$BUILD/_deps/frozen-src/include" \
    "$BUILD/libconfig-option-lib-cxxbridge.a" \
    "$BUILD/libconfig_option_lib.a" \
    "$BUILD/corrosion_generated/cxxbridge/config-option-lib-cxxbridge/include"
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

OUT="/tmp/km-test-k21"
LOG="$OUT-build.log"
: > "$LOG"

/usr/bin/c++ \
    -DKM_HELPER_DIR=\"/usr/lib/kernel-manager\" \
    -DKM_IGNORE_REPO=\"\" \
    -I"$ROOT/src" \
    -I"$BUILD" \
    -I"$BUILD/_deps/fmt-src/include" \
    -I"$BUILD/_deps/frozen-src/include" \
    -I"$BUILD/corrosion_generated/cxxbridge/config-option-lib-cxxbridge/include" \
    -isystem /usr/include/glib-2.0 \
    -isystem /usr/lib/glib-2.0/include \
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
    tests/test_k21_config_options.cpp \
    src/config-options.cpp \
    "$BUILD/_deps/fmt-build/libfmt.a" \
    "$BUILD/libconfig-option-lib-cxxbridge.a" \
    "$BUILD/libconfig_option_lib.a" \
    -lgcc_s -lutil -lrt -lpthread -lm -ldl -lc \
    /usr/lib/libglib-2.0.so \
    -o "$OUT" 2>> "$LOG" \
    || { echo "error: k21 compile failed:" >&2; tail -n 40 "$LOG" >&2; exit 1; }

# Warning gate: 0 source warnings. The lto-wrapper serial-compilation
# note is tooling, not code (same exclusion as k12). The GCC-16
# -Wnull-dereference false positives in /usr/include/c++/16/streambuf
# (triggered by the fmt include chain) are system-header noise, not
# project code — exclude them the way k20 excludes the same class of
# diagnostic (4 header warnings, zero project-code warnings).
SRC_WARNINGS="$(grep 'warning:' "$LOG" | grep -v 'lto-wrapper:' | grep -vc '/usr/include/' || true)"
if [[ "$SRC_WARNINGS" -gt 0 ]]; then
    echo "error: expected 0 source warnings, got $SRC_WARNINGS:" >&2
    grep 'warning:' "$LOG" | grep -v 'lto-wrapper:' | grep -v '/usr/include/' >&2 || true
    exit 1
fi
echo "INFO: build clean (source warnings=0)"

"$OUT"

# Remove our own artifacts before the leak check (the test removes its own
# mkstemp sandbox; the build log and binary are ours).
rm -f "$OUT" "$LOG"

# Cleanup gate: verify no stray /tmp/km-test-k21-* directories leaked
# (the test's sandbox is named km-test-k21-<pid>, ours are km-test-k21 and
# km-test-k21-build.log — both removed above).
if ls -d /tmp/km-test-k21-* 1>/dev/null 2>&1; then
    echo "error: k21 sandbox leak:" >&2
    ls -d /tmp/km-test-k21-* >&2
    exit 1
fi

echo "K21: OK"
