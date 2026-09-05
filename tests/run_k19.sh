#!/usr/bin/env bash
# Standalone K19 unit test (no CTest infra in this project; follows the
# "standalone g++ on the real sources" precedent of tests/run_k18.sh).
# The driver_gate module is pure C++ (no Qt, no alpm, no fmt — std-only),
# so this compiles the real source with the project's GCC warning set and
# runs the D3 decision-table assertions against injected GateProbes only.
#
# Recipe shape (one forced deviation from k18's single driver command):
# the two TUs are compiled separately with the k18 flag set (incl.
# -O3 -flto -fwhole-program), then linked with LTO WITHOUT the -W flags —
# exactly the canonical cmake pipeline shape. A single command would carry
# the -W flags onto the link-time LTRANS whole-program pass, where GCC 16's
# -Wnull-dereference re-analysis flags a libstdc++ istreambuf_iterator
# false positive in src/driver_gate.cpp's read_small_file (4 header
# warnings, zero project-code warnings) — the harness's real-probe smoke
# makes the module's probes live, while the cmake build leaves them dead
# (no app caller until chunks 3/4) and its link line carries no -W flags,
# so the cmake build stays 0-warning.
#
# Gates:
#   1. 0 source warnings in the compile phase (the lto-wrapper
#      serial-LTRANS line is tooling, not code)
#   2. exit 0 (the k18-style check() count; a non-zero rc aborts under
#      set -e)
#   3. the live /etc/pacman.conf md5 before/after (the harness is
#      read-only — the real-probe smoke runs lspci/pacman -Siq without
#      root; the md5 gate proves it, the k13/k14 pattern)
set -euo pipefail
cd "$(dirname "$0")/.."

OUT="/tmp/km-test-k19"
LOG="/tmp/km-test-k19-build.log"

FLAGS="-O3 -flto -fwhole-program -fuse-linker-plugin \
  -std=c++23 -fPIE -fdiagnostics-color=always -mno-direct-extern-access \
  -Wall -Wextra -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual \
  -Wpedantic -Wconversion -Wsign-conversion -Wnull-dereference -Wdouble-promotion -Wformat=2 \
  -Wimplicit-fallthrough -Wmisleading-indentation -Wduplicated-cond -Wduplicated-branches -Wlogical-op \
  -Wuseless-cast -Wsuggest-attribute=cold -Wsuggest-attribute=format -Wsuggest-attribute=malloc \
  -Wsuggest-attribute=noreturn -Wsuggest-attribute=pure -Wsuggest-final-methods \
  -Wsuggest-final-types -Wdiv-by-zero -Wanalyzer-double-fclose -Wanalyzer-double-free \
  -Wanalyzer-malloc-leak -Wanalyzer-use-after-free \
  -D_FILE_OFFSET_BITS=64 -pthread"

/usr/bin/c++ $FLAGS -Isrc -c tests/test_k19_driver_gate.cpp -o "$OUT.t.o" 2> "$LOG" \
    || { echo "error: k19 compile of the test TU failed:" >&2; cat "$LOG" >&2; exit 1; }
/usr/bin/c++ $FLAGS -Isrc -c src/driver_gate.cpp -o "$OUT.d.o" 2>> "$LOG" \
    || { echo "error: k19 compile of the module TU failed:" >&2; cat "$LOG" >&2; exit 1; }
/usr/bin/c++ -O3 -flto -fuse-linker-plugin -fPIE "$OUT.t.o" "$OUT.d.o" -o "$OUT" 2>> "$LOG" \
    || { echo "error: k19 LTO link failed:" >&2; cat "$LOG" >&2; exit 1; }
rm -f "$OUT.t.o" "$OUT.d.o"

# Warning gate: 0 warnings beyond the lto-wrapper tooling line.
if [[ "$(grep 'warning:' "$LOG" | grep -vc 'lto-wrapper:' || true)" -gt 0 ]]; then
    echo "error: k19 build produced source warnings:" >&2
    grep 'warning:' "$LOG" | grep -v 'lto-wrapper:' >&2 || true
    exit 1
fi

# Read-only proof: snapshot the live conf before the (real-probe) run.
ETC_MD5_BEFORE="$(md5sum /etc/pacman.conf 2>/dev/null | awk '{print $1}' || true)"

# The exit-0 check: under set -e a non-zero rc aborts the script here.
"$OUT"

ETC_MD5_AFTER="$(md5sum /etc/pacman.conf 2>/dev/null | awk '{print $1}' || true)"
if [[ -n "$ETC_MD5_BEFORE" && "$ETC_MD5_BEFORE" != "$ETC_MD5_AFTER" ]]; then
    echo "error: the k19 harness changed /etc/pacman.conf (before=$ETC_MD5_BEFORE after=$ETC_MD5_AFTER)" >&2
    exit 1
fi
echo "K19: OK (0 warnings, exit 0, /etc/pacman.conf untouched)"
