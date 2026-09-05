#!/usr/bin/env bash
# k12: offscreen render harness for the kernel tree's Choose cell (D3 lock
# glyph), Version cell (D4 "— (repo not enabled)" annotation), and the
# "Install from directory…" pseudo-row (C3, plan v1.24.0: the 25th row —
# folder-glyph QLabel, no checkbox, the D1 tooltips — plus the menu
# probes: the directory row's one-action menu, never clicked, and a
# data-row no-regression shape check) — see
# tests/test_k12_tree_render.cpp (no CTest infra in this project; follows
# the tests/run_chunk2_ui.sh + run_k11.sh recipes: compiles the REAL
# MainWindow source closure with the project's GCC warning set, links fmt +
# the cxxbridge Rust libs + glib + Qt6 + libalpm, drives the real window
# offscreen, never writes — alpm opened read-only, no root, no terminal,
# menu actions never clicked, /etc/pacman.conf untouched). install_kernel.cpp
# resolves its D6 pairing default to driver_gate (chunk 5), so
# src/driver_gate.cpp is in the compile list (the k8/k14 companion).
#
# Recipe shape (one forced deviation from the single driver command, the
# k19 precedent): the TUs are compiled separately with the project's GCC
# warning set, then linked with LTO WITHOUT the -W flags — exactly the
# canonical cmake pipeline shape. A single command would carry the -W flags
# onto the link-time LTRANS whole-program pass, where GCC 16's
# -Wnull-dereference re-analysis flags libstdc++ istreambuf_iterator false
# positives in src/driver_gate.cpp's read_small_file (4 header warnings,
# zero project-code warnings) now that chunk 3 makes the module's probes
# live in the km-window.cpp closure (the cmake build's link line carries
# no -W flags, so it stays 0-warning; separate TUs compile clean — proven
# per-TU before this split).
#
# Gates:
#   1. the harness exits 0 with all assertions passing (both runs)
#   2. 0 source warnings (cycle-7 C7a eliminated the -Wignored-attributes
#      baseline)
#   3. the K12-DUMP row dump is byte-stable across two runs
#      (deterministic order)
set -euo pipefail
cd "$(dirname "$0")/.."

ROOT="$(pwd)"
BUILD="$ROOT/build"

# The harness needs the fmt + frozen + cxxbridge + codegen + autogen
# artifacts from the project's CMake build dir (the autogen tree carries
# both the uic-generated ui_* headers and the moc files). If this worktree
# has no build/ of its own (or any piece is absent), copy them from the
# main worktree's build — the CMake configure is their only producer and
# they are version-pinned (run_k11 precedent).
for f in \
    "$BUILD/_deps/fmt-src/include" \
    "$BUILD/_deps/fmt-build/libfmt.a" \
    "$BUILD/_deps/frozen-src/include" \
    "$BUILD/libconfig-option-lib-cxxbridge.a" \
    "$BUILD/libconfig_option_lib.a" \
    "$BUILD/compile_options.hpp" \
    "$BUILD/corrosion_generated" \
    "$BUILD/kernel-manager_autogen"
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

# The autogen hash dir differs per build tree; locate the mocs (the classes
# are Q_OBJECTs — their vtables/meta-objects live in the generated mocs,
# which the MainWindow/ConfWindow constructors need at link time).
MOC_KM_WINDOW="$(ls "$BUILD/kernel-manager_autogen"/*/moc_km-window.cpp 2>/dev/null | head -n1 || true)"
MOC_CONF_WINDOW="$(ls "$BUILD/kernel-manager_autogen"/*/moc_conf-window.cpp 2>/dev/null | head -n1 || true)"
MOC_CONF_OPTIONS_PAGE="$(ls "$BUILD/kernel-manager_autogen"/*/moc_conf-options-page.cpp 2>/dev/null | head -n1 || true)"
MOC_CONF_PATCHES_PAGE="$(ls "$BUILD/kernel-manager_autogen"/*/moc_conf-patches-page.cpp 2>/dev/null | head -n1 || true)"
if [[ -z "${MOC_KM_WINDOW}" || -z "${MOC_CONF_WINDOW}" || -z "${MOC_CONF_OPTIONS_PAGE}" || -z "${MOC_CONF_PATCHES_PAGE}" ]]; then
    echo "error: km-window/conf-window/page mocs not found under $BUILD/kernel-manager_autogen (run the CMake build first)" >&2
    exit 1
fi

# The app reads its version from the APP_VERSION compile definition (CMake
# exposes project VERSION to km-window.cpp). Derive the same value from
# CMakeLists.txt (the single source of truth) so the standalone compile
# matches the CMake build.
APP_VERSION="$(sed -n 's/^ *VERSION \([0-9][0-9.]*\).*$/\1/p' "$ROOT/CMakeLists.txt" | head -n1)"
if [[ -z "$APP_VERSION" ]]; then
    echo "error: could not derive APP_VERSION from CMakeLists.txt (project VERSION line)" >&2
    exit 1
fi

OUT="/tmp/km-test-k12"
LOG="/tmp/km-test-k12-build.log"

# The shared compiler settings: the original single command's flags,
# verbatim, split into the compile phase (with the warning set) and the
# LTO-link phase (without — the canonical cmake shape).
COMPILE_FLAGS="-O3 -flto -fwhole-program -fuse-linker-plugin \
    -std=c++23 -fPIE -fdiagnostics-color=always -mno-direct-extern-access \
    -Wall -Wextra -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual \
    -Wpedantic -Wconversion -Wsign-conversion -Wnull-dereference -Wdouble-promotion -Wformat=2 \
    -Wimplicit-fallthrough -Wmisleading-indentation -Wduplicated-cond -Wduplicated-branches -Wlogical-op \
    -Wuseless-cast -Wsuggest-attribute=cold -Wsuggest-attribute=format -Wsuggest-attribute=malloc \
    -Wsuggest-attribute=noreturn -Wsuggest-attribute=pure -Wsuggest-final-methods \
    -Wsuggest-final-types -Wdiv-by-zero -Wanalyzer-double-fclose -Wanalyzer-double-free \
    -Wanalyzer-malloc-leak -Wanalyzer-use-after-free \
    -D_FILE_OFFSET_BITS=64 -pthread"

INCLUDES="-DAPP_VERSION=\"$APP_VERSION\" \
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
    -isystem /usr/include/sysprof-6"

# Phase 1: compile every TU separately with the project's GCC warning set
# (the per-TU passes are clean — it is the LTRANS whole-program pass that
# re-flags the libstdc++ istreambuf_iterator false positives).
TUS=(
    tests/test_k12_tree_render.cpp
    src/km-window.cpp
    src/conf-window.cpp
    src/kernel.cpp
    src/known_kernels.cpp
    src/alpm_utils.cpp
    src/utils.cpp
    src/install_kernel.cpp
    src/driver_gate.cpp
    src/aur_kernel.cpp
    src/boot_instructions.cpp
    src/bootloader.cpp
    src/config-options.cpp
    "$MOC_KM_WINDOW"
    "$MOC_CONF_WINDOW"
    "$MOC_CONF_OPTIONS_PAGE"
    "$MOC_CONF_PATCHES_PAGE"
)
OBJS=()
: > "$LOG"
for tu in "${TUS[@]}"; do
    obj="/tmp/km-test-k12-$(printf '%s' "$tu" | tr '/.' '__').o"
    /usr/bin/c++ $INCLUDES $COMPILE_FLAGS -c "$tu" -o "$obj" 2>> "$LOG" \
        || { echo "error: k12 compile of $tu failed:" >&2; tail -n 30 "$LOG" >&2; exit 1; }
    OBJS+=("$obj")
done

# Phase 2: the LTO link WITHOUT the -W flags (the link line never carries
# code diagnostics — the cmake pipeline shape).
/usr/bin/c++ -O3 -flto -fuse-linker-plugin -fPIE "${OBJS[@]}" \
    "$BUILD/_deps/fmt-build/libfmt.a" \
    "$BUILD/libconfig-option-lib-cxxbridge.a" \
    "$BUILD/libconfig_option_lib.a" \
    -lgcc_s -lutil -lrt -lpthread -lm -ldl -lc \
    /usr/lib/libglib-2.0.so \
    /usr/lib/libQt6Widgets.so \
    /usr/lib/libQt6Gui.so \
    /usr/lib/libQt6Core.so \
    -lalpm \
    -o "$OUT" 2>> "$LOG" \
    || { echo "error: k12 LTO link failed:" >&2; tail -n 30 "$LOG" >&2; exit 1; }
rm -f "${OBJS[@]}"

# Warning gate: 0 source warnings (cycle-7 C7a eliminated the
# -Wignored-attributes baseline; the
# lto-wrapper "warning: using serial compilation of N LTRANS jobs" line is
# a toolchain note — the same diagnostic CMake builds report as "note:" —
# and is not a source diagnostic, so it is excluded from the count).
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

# Run twice: all assertions green (exit 0 = zero failures) + the K12-DUMP
# row dump byte-stable across the runs (deterministic order).
RUN1="/tmp/km-test-k12-run1.log"
RUN2="/tmp/km-test-k12-run2.log"
set +e
QT_QPA_PLATFORM=offscreen "$OUT" > "$RUN1"
RC1=$?
set -e
cat "$RUN1"
if [[ "$RC1" -ne 0 ]]; then
    echo "error: k12 run 1 failed (rc=$RC1)" >&2
    exit 1
fi

set +e
QT_QPA_PLATFORM=offscreen "$OUT" > "$RUN2"
RC2=$?
set -e
cat "$RUN2"
if [[ "$RC2" -ne 0 ]]; then
    echo "error: k12 run 2 failed (rc=$RC2)" >&2
    exit 1
fi

DUMP1="/tmp/km-test-k12-dump1"
DUMP2="/tmp/km-test-k12-dump2"
grep '^K12-DUMP:' "$RUN1" > "$DUMP1"
grep '^K12-DUMP:' "$RUN2" > "$DUMP2"
N1="$(wc -l < "$DUMP1")"
N2="$(wc -l < "$DUMP2")"
if [[ "$N1" -eq 0 || "$N1" -ne "$N2" ]]; then
    echo "error: row dump missing or unstable across runs ($N1 vs $N2 lines)" >&2
    exit 1
fi
if ! cmp -s "$DUMP1" "$DUMP2"; then
    echo "error: row dump not byte-stable across two runs:" >&2
    diff "$DUMP1" "$DUMP2" >&2 || true
    exit 1
fi
echo "INFO: row dump stable across two runs ($N1 rows)"
echo "K12: OK (run1 rc=$RC1, run2 rc=$RC2; dump $N1 rows stable)"
