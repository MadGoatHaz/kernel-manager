#!/usr/bin/env bash
# k13: harness for the C7 repo-add (C4 C++ contract + C6 script paths) — see
# tests/test_k13_repo_add.cpp (no CTest infra in this project; follows the
# tests/run_k7.sh standalone-g++ recipe + C6's no-root script verification).
# Two parts:
#
#   Part A (C++): compiles the real src/alpm_utils.cpp (+ src/utils.cpp,
#   whose exec() backs the AUR probe, + src/known_kernels.cpp for the
#   curated-table guard) with the project's GCC warning set, links fmt +
#   glib + Qt6Core + libalpm. Asserts is_repo_enabled (temp confs +
#   live-conf spot-checks) and add_repo_to_pacman_conf with a fake
#   recording runner (launch contract + all guard rejections + rc
#   propagation). Part A reads /etc/pacman.conf only; it never launches a
#   real terminal or modifies anything.
#
#   Part B (bash): exercises the real src/repo_add.sh without root, every
#   write redirected via `--conf <temp file>` into an mktemp sandbox, with a
#   `pacman` stub (and a `pacman-key` stub) on PATH that record their calls.
#   Verifies: (i) a fresh conf gets exactly one [cachyos] section with
#   literal $arch/$repo Server lines appended after a single .bak-<epoch>
#   backup, and pacman is called once with -Sy; (ii) a second run is
#   idempotent (conf byte-identical, no new backup, "already enabled",
#   pacman re-run); (iii) --dry-run prints the section + the would-run plan,
#   leaves the conf untouched, creates no backup, and never calls pacman;
#   (iv) rejections (no args / unknown repo / metacharacter repo / --conf
#   missing value) exit non-zero and leave the conf untouched; (v) a
#   one-shot GPG failure (first `pacman -Sy` exits with the real-world
#   unknown-key error) on top of the PROACTIVE import+trust pair (pacman-key
#   --recv-keys + --lsign-key with the allowlist's 40-hex REPO_KEY, run
#   before every real `pacman -Sy`) makes the reactive case auto-import the
#   key (same pair with the extracted 40-hex ID) and retry, on BOTH the
#   fresh and the already-enabled paths (exit 0 when the retry succeeds);
#   (vi) the same failure with EVERY pacman-key call failing exits non-zero
#   without a retry (the unrepaired error surfaces as-is, the reactive
#   --lsign-key short-circuited by &&); (vii) a one-shot unknown-TRUST
#   failure (the -Sy error names the signer's uid, NOT a key ID — the case
#   the key-ID parse cannot see) makes reactive case-2 look the key up via
#   `pacman-key --list-keys <uid>`, lsign the found 16-hex primary ID,
#   retry, and exit 0.
#
# Safety: the live /etc/pacman.conf is NEVER modified by this harness — every
# non-dry run uses `--conf <temp file>`, so all writes land in the sandbox;
# by construction no write can reach /etc/ (and the before/after md5 below
# proves it on this machine).
#
# Gates:
#   1. Part A compiles with at most ONE source warning, and it must be the
#      pre-existing utils.cpp:103 -Wignored-attributes baseline (k7/k12
#      precedent), and the test binary exits 0
#   2. Part B: all script-path assertions pass
set -euo pipefail
cd "$(dirname "$0")/.."

ROOT="$(pwd)"
BUILD="$ROOT/build"
SCRIPT="$ROOT/src/repo_add.sh"

# --- bootstrap the fmt artifacts (run_k12 pattern) ------------------------
# Part A needs the fmt include dir + static lib from the project's CMake
# build dir (the CMake configure is their only producer; they are
# version-pinned). If this worktree has no build/ of its own, copy them
# from the main worktree's build.
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

# --- Part A: compile + run the C++ contract test (k7 recipe) --------------
OUT="/tmp/km-test-k13"
LOG="/tmp/km-test-k13-build.log"

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
    -std=c++23 -fPIE -fdiagnostics-color=always -mno-direct-extern-access \
    -Wall -Wextra -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused -Woverloaded-virtual \
    -Wpedantic -Wconversion -Wsign-conversion -Wnull-dereference -Wdouble-promotion -Wformat=2 \
    -Wimplicit-fallthrough -Wmisleading-indentation -Wduplicated-cond -Wduplicated-branches -Wlogical-op \
    -Wuseless-cast -Wsuggest-attribute=cold -Wsuggest-attribute=format -Wsuggest-attribute=malloc \
    -Wsuggest-attribute=noreturn -Wsuggest-attribute=pure -Wsuggest-final-methods \
    -Wsuggest-final-types -Wdiv-by-zero -Wanalyzer-double-fclose -Wanalyzer-double-free \
    -Wanalyzer-malloc-leak -Wanalyzer-use-after-free \
    -D_FILE_OFFSET_BITS=64 -pthread \
    tests/test_k13_repo_add.cpp \
    src/alpm_utils.cpp \
    src/utils.cpp \
    src/known_kernels.cpp \
    "$BUILD/_deps/fmt-build/libfmt.a" \
    -lgcc_s -lutil -lrt -lpthread -lm -ldl -lc \
    /usr/lib/libglib-2.0.so \
    /usr/lib/libQt6Core.so \
    -lalpm \
    -o "$OUT" 2> "$LOG"

# Warning gate: at most one SOURCE warning, and it must be the pre-existing
# utils.cpp:103 -Wignored-attributes baseline (run_k7/k12 precedent; the
# lto-wrapper "serial compilation of N LTRANS jobs" line is a toolchain note
# — the same diagnostic CMake builds report as "note:" — not a source
# diagnostic, so it is excluded from the count).
SRC_WARNINGS="$(grep 'warning:' "$LOG" | grep -vc 'lto-wrapper:' || true)"
BASELINE_WARNINGS="$(grep -c 'utils\.cpp:103.*-Wignored-attributes' "$LOG" || true)"
if [[ "$SRC_WARNINGS" -gt 1 ]]; then
    echo "error: $SRC_WARNINGS source warnings (expected at most 1 = the utils.cpp:103 baseline):" >&2
    grep 'warning:' "$LOG" | grep -v 'lto-wrapper:' >&2 || true
    exit 1
fi
if [[ "$SRC_WARNINGS" -eq 1 && "$BASELINE_WARNINGS" -ne 1 ]]; then
    echo "error: the single source warning is NOT the utils.cpp:103 baseline:" >&2
    grep 'warning:' "$LOG" | grep -v 'lto-wrapper:' >&2 || true
    exit 1
fi
echo "INFO: build clean (source warnings=$SRC_WARNINGS; 1 = the utils.cpp:103 baseline)"

# The harness is read-only with respect to the system (no event loop needed
# — Part A drives the alpm_utils surface directly).
set +e
"$OUT" > "/tmp/km-test-k13-run.log"
RC_A=$?
set -e
cat "/tmp/km-test-k13-run.log"
if [[ "$RC_A" -ne 0 ]]; then
    echo "error: k13 part A (C++ contract) failed (rc=$RC_A)" >&2
    exit 1
fi
echo "INFO: part A green (rc=0)"

# --- Part B: the script's paths (no root; sandboxed writes) ---------------
KB="$(mktemp -d /tmp/km-k13.XXXXXX)"
trap 'rm -rf "$KB"' EXIT

# Belt and braces: prove the live conf is untouched by the whole run.
ETC_MD5_BEFORE="$(md5sum /etc/pacman.conf 2>/dev/null | awk '{print $1}' || true)"

# The `pacman` stub: records every invocation (args joined) to $PACMAN_LOG.
# By default it exits 0. Two one-shot tripwires (each points at a
# not-yet-existing file; only -Sy is affected; the file is created on the
# first trip = consumed, so a retry exits 0):
#   PACMAN_FAIL_GPG   — the real-world unknown-KEY error (names the 40-hex
#                       ID) — the B6/B7/B8 reactive case-1 auto-import
#   PACMAN_FAIL_TRUST — the real-world unknown-TRUST error (names the
#                       signer's uid, NO key ID) — the B9 reactive case-2
#                       lookup+sign. It never touches the system.
mkdir -p "$KB/bin"
PACMAN_LOG="$KB/pacman.log"
PACMAN_KEY_LOG="$KB/pacman-key.log"
: > "$PACMAN_LOG"
: > "$PACMAN_KEY_LOG"
cat > "$KB/bin/pacman" <<'EOF'
#!/bin/bash
echo "pacman $*" >> "$PACMAN_LOG"
if [[ -n "${PACMAN_FAIL_GPG:-}" && "$1" == "-Sy" && ! -e "$PACMAN_FAIL_GPG" ]]; then
    touch "$PACMAN_FAIL_GPG"
    echo "error: cachyos: key \"882DCFE48E2051D48E2562ABF3B607488DB35A47\" is unknown" >&2
    exit 1
fi
if [[ -n "${PACMAN_FAIL_TRUST:-}" && "$1" == "-Sy" && ! -e "$PACMAN_FAIL_TRUST" ]]; then
    touch "$PACMAN_FAIL_TRUST"
    echo "error: cachyos: signature from \"CachyOS <repo@cachyos.org>\" is unknown trust" >&2
    exit 1
fi
exit 0
EOF
chmod +x "$KB/bin/pacman"
# The `pacman-key` stub: records every invocation to $PACMAN_KEY_LOG and
# exits 0, except when PACMAN_KEY_FAIL is set, in which case EVERY call
# fails (the B8 unrepaired-import case; the flag file is just the trigger).
# A `--list-keys` call (reactive case-2, B9) additionally prints a fake
# keyring block in gpg's human format — first 16-hex line = the primary
# key ID, followed by the signer uid — so the script's uid→ID lookup
# resolves deterministically.
cat > "$KB/bin/pacman-key" <<'EOF'
#!/bin/bash
echo "pacman-key $*" >> "$PACMAN_KEY_LOG"
if [[ -n "${PACMAN_KEY_FAIL:-}" ]]; then
    touch "$PACMAN_KEY_FAIL"
    echo "error: gpg: no key found" >&2
    exit 1
fi
if [[ "$1" == "--list-keys" ]]; then
    printf 'pub   rsa4096 2022-05-23 [SC]\n      D6C9442437365605\nuid           [ unknown] TNE <tne@garudalinux.org>\n'
fi
exit 0
EOF
chmod +x "$KB/bin/pacman-key"
export PACMAN_LOG PACMAN_KEY_LOG
PATH="$KB/bin:$PATH"

FAILURES=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1"; FAILURES=$((FAILURES + 1)); }

# Runs the real script, capturing combined output; never trips set -e.
SCRIPT_OUT=""
SCRIPT_RC=0
run_repo_add() {
    set +e
    SCRIPT_OUT="$("$SCRIPT" "$@" 2>&1)"
    SCRIPT_RC=$?
    set -e
    return 0
}

# A fresh minimal conf (unique per test case).
fresh_conf() {
    printf '[options]\nNoUpgrade = usr/\n\n[core]\nInclude = /etc/pacman.d/mirrorlist\n' > "$1"
}

pacman_sy_count() { grep -c 'pacman -Sy' "$PACMAN_LOG" || true; }
# `ls` exits 2 when the glob matches nothing (the no-backup cases) — under
# set -o pipefail the || true keeps the "0" output without aborting the run.
backup_count() { ls "$1".bak-* 2>/dev/null | wc -l || true; }

if [[ -x "$SCRIPT" ]]; then pass "B0: repo_add.sh is executable"; else fail "B0: repo_add.sh not executable"; fi
if bash -n "$SCRIPT" 2>/dev/null; then pass "B0: bash -n clean"; else fail "B0: bash -n reports a syntax error"; fi

# (i) fresh conf: one backup, one [cachyos] section, pacman -Sy once.
CONF1="$KB/conf1"
fresh_conf "$CONF1"
cp "$CONF1" "$KB/conf1.orig"
run_repo_add cachyos --conf "$CONF1"
if [[ "$SCRIPT_RC" -eq 0 ]]; then pass "B1: fresh run exits 0"; else fail "B1: fresh run rc=$SCRIPT_RC: $SCRIPT_OUT"; fi
case "$SCRIPT_OUT" in
    "Backed up "*) pass "B1: backup announced before the append";;
    *) fail "B1: no backup announcement: $SCRIPT_OUT";;
esac
case "$SCRIPT_OUT" in
    *"Appended [cachyos] to"*) pass "B1: append announced";;
    *) fail "B1: no append announcement: $SCRIPT_OUT";;
esac
case "$SCRIPT_OUT" in
    *"Running pacman -Sy"*) pass "B1: pacman -Sy announced";;
    *) fail "B1: no pacman -Sy announcement: $SCRIPT_OUT";;
esac
N_SECTIONS="$(grep -c '^\[cachyos\]$' "$CONF1" || true)"
if [[ "$N_SECTIONS" -eq 1 ]]; then pass "B1: exactly one [cachyos] section appended"; else fail "B1: expected 1 [cachyos] header line, got $N_SECTIONS"; fi
N_SERVERS="$(grep -A2 '^\[cachyos\]$' "$CONF1" | grep -c 'Server = .*/\$arch/\$repo$' || true)"
if [[ "$N_SERVERS" -eq 2 ]]; then pass "B1: both Server lines carry literal \$arch/\$repo (pacman expands, bash never did)"; else fail "B1: expected 2 literal \$arch/\$repo Server lines, got $N_SERVERS"; fi
N_BACKUPS="$(backup_count "$CONF1")"
if [[ "$N_BACKUPS" -eq 1 ]]; then pass "B1: exactly one .bak-<epoch> created"; else fail "B1: expected 1 backup, got $N_BACKUPS"; fi
BAK1="$(ls "$CONF1".bak-* 2>/dev/null | head -n1 || true)"
if [[ -n "$BAK1" ]] && cmp -s "$BAK1" "$KB/conf1.orig"; then pass "B1: the backup is the pre-append conf (byte-identical)"; else fail "B1: backup missing or not the pre-append conf"; fi
N_SY="$(pacman_sy_count)"
if [[ "$N_SY" -eq 1 ]]; then pass "B1: pacman stub called exactly once with -Sy"; else fail "B1: expected 1 pacman -Sy call, got $N_SY"; fi

# (ii) second run: idempotent (conf unchanged, no new backup, pacman re-run).
cp "$CONF1" "$KB/conf1.after1"
run_repo_add cachyos --conf "$CONF1"
if [[ "$SCRIPT_RC" -eq 0 ]]; then pass "B2: second run exits 0"; else fail "B2: second run rc=$SCRIPT_RC: $SCRIPT_OUT"; fi
case "$SCRIPT_OUT" in
    *"already enabled"*) pass "B2: 'already enabled' printed";;
    *) fail "B2: no 'already enabled' in output: $SCRIPT_OUT";;
esac
if cmp -s "$CONF1" "$KB/conf1.after1"; then pass "B2: conf byte-identical after the second run"; else fail "B2: conf changed on the second run"; fi
N_BACKUPS="$(backup_count "$CONF1")"
if [[ "$N_BACKUPS" -eq 1 ]]; then pass "B2: no new backup (still one)"; else fail "B2: expected 1 backup after the second run, got $N_BACKUPS"; fi
N_SY="$(pacman_sy_count)"
if [[ "$N_SY" -eq 2 ]]; then pass "B2: pacman stub called with -Sy again (total 2)"; else fail "B2: expected 2 pacman -Sy calls, got $N_SY"; fi

# (iii) --dry-run on a fresh conf: plan printed, no writes, no pacman.
CONF2="$KB/conf2"
fresh_conf "$CONF2"
cp "$CONF2" "$KB/conf2.orig"
run_repo_add cachyos --conf "$CONF2" --dry-run
if [[ "$SCRIPT_RC" -eq 0 ]]; then pass "B3: dry-run exits 0"; else fail "B3: dry-run rc=$SCRIPT_RC: $SCRIPT_OUT"; fi
case "$SCRIPT_OUT" in
    *"[dry-run] would append to"*) pass "B3: would-append plan printed";;
    *) fail "B3: no would-append plan: $SCRIPT_OUT";;
esac
N_DRY_SERVERS="$(printf '%s\n' "$SCRIPT_OUT" | grep -A2 '^\[cachyos\]$' | grep -c 'Server = .*/\$arch/\$repo$' || true)"
if [[ "$N_DRY_SERVERS" -eq 2 ]]; then pass "B3: the printed section carries both literal \$arch/\$repo Server lines"; else fail "B3: expected 2 Server lines in the printed section, got $N_DRY_SERVERS"; fi
case "$SCRIPT_OUT" in
    *"[dry-run] would run: pacman -Sy"*) pass "B3: would-run pacman -Sy printed";;
    *) fail "B3: no would-run pacman -Sy: $SCRIPT_OUT";;
esac
if cmp -s "$CONF2" "$KB/conf2.orig"; then pass "B3: conf untouched by the dry-run"; else fail "B3: dry-run modified the conf"; fi
N_BACKUPS="$(backup_count "$CONF2")"
if [[ "$N_BACKUPS" -eq 0 ]]; then pass "B3: no backup created by the dry-run"; else fail "B3: dry-run created a backup"; fi
N_SY="$(pacman_sy_count)"
if [[ "$N_SY" -eq 2 ]]; then pass "B3: pacman stub never called by the dry-run (still 2)"; else fail "B3: expected 2 pacman -Sy calls, got $N_SY"; fi

# (iv) rejections: non-zero exit, conf untouched, no backups, no pacman.
CONF3="$KB/conf3"
fresh_conf "$CONF3"
cp "$CONF3" "$KB/conf3.orig"

run_repo_add
if [[ "$SCRIPT_RC" -ne 0 ]]; then pass "B4: no args → non-zero exit"; else fail "B4: no args exited 0"; fi
case "$SCRIPT_OUT" in
    *"no repo given"*) pass "B4: 'no repo given' error printed";;
    *) fail "B4: no 'no repo given' error: $SCRIPT_OUT";;
esac

run_repo_add bogus --conf "$CONF3"
if [[ "$SCRIPT_RC" -ne 0 ]]; then pass "B4: 'bogus' (allowlist-absent) → non-zero exit"; else fail "B4: 'bogus' exited 0"; fi
case "$SCRIPT_OUT" in
    *"unknown repo 'bogus'"*) pass "B4: 'unknown repo' error printed";;
    *) fail "B4: no 'unknown repo' error: $SCRIPT_OUT";;
esac

run_repo_add 'ca;ch;os' --conf "$CONF3"
if [[ "$SCRIPT_RC" -ne 0 ]]; then pass "B4: 'ca;ch;os' (metacharacter) → non-zero exit"; else fail "B4: 'ca;ch;os' exited 0"; fi

run_repo_add cachyos --conf
if [[ "$SCRIPT_RC" -ne 0 ]]; then pass "B4: '--conf' with no value → non-zero exit"; else fail "B4: '--conf' with no value exited 0"; fi
case "$SCRIPT_OUT" in
    *"--conf requires a path"*) pass "B4: '--conf requires a path' error printed";;
    *) fail "B4: no '--conf requires a path' error: $SCRIPT_OUT";;
esac

if cmp -s "$CONF3" "$KB/conf3.orig"; then pass "B4: conf untouched by every rejection"; else fail "B4: a rejection modified the conf"; fi
N_BACKUPS="$(backup_count "$CONF3")"
if [[ "$N_BACKUPS" -eq 0 ]]; then pass "B4: no backups created by the rejections"; else fail "B4: rejections created backups"; fi
N_SY="$(pacman_sy_count)"
if [[ "$N_SY" -eq 2 ]]; then pass "B4: pacman stub never called by the rejections (still 2)"; else fail "B4: expected 2 pacman -Sy calls, got $N_SY"; fi

# (v) GPG auto-import on the FRESH path: the one-shot GPG failure makes the
# first `pacman -Sy` exit non-zero ⇒ the script must extract the key ID,
# call pacman-key --recv-keys + --lsign-key with it, retry `pacman -Sy`,
# and exit 0 when the retry succeeds.
GPG_ID="882DCFE48E2051D48E2562ABF3B607488DB35A47"
CONF4="$KB/conf4"
fresh_conf "$CONF4"
cp "$CONF4" "$KB/conf4.orig"
SY_BEFORE="$(pacman_sy_count)"
KEY_BEFORE="$(grep -c 'pacman-key' "$PACMAN_KEY_LOG" || true)"
export PACMAN_FAIL_GPG="$KB/gpg1"
run_repo_add cachyos --conf "$CONF4"
unset PACMAN_FAIL_GPG
if [[ "$SCRIPT_RC" -eq 0 ]]; then pass "B6: GPG-failure fresh run exits 0 (the retry succeeded)"; else fail "B6: GPG-failure fresh run rc=$SCRIPT_RC: $SCRIPT_OUT"; fi
case "$SCRIPT_OUT" in
    *"Importing GPG key $GPG_ID"*) pass "B6: 'Importing GPG key <id>' announced";;
    *) fail "B6: no GPG-key import announcement: $SCRIPT_OUT";;
esac
case "$SCRIPT_OUT" in
    *"Retrying pacman -Sy"*) pass "B6: 'Retrying pacman -Sy…' announced";;
    *) fail "B6: no retry announcement: $SCRIPT_OUT";;
esac
if grep -qx "pacman-key --recv-keys $GPG_ID" "$PACMAN_KEY_LOG"; then pass "B6: pacman-key --recv-keys called with the extracted key ID"; else fail "B6: pacman-key --recv-keys <id> missing"; fi
if grep -qx "pacman-key --lsign-key $GPG_ID" "$PACMAN_KEY_LOG"; then pass "B6: pacman-key --lsign-key called with the extracted key ID"; else fail "B6: pacman-key --lsign-key <id> missing"; fi
N_SY="$(pacman_sy_count)"
if [[ "$N_SY" -eq $((SY_BEFORE + 2)) ]]; then pass "B6: pacman -Sy called twice (initial + retry)"; else fail "B6: expected 2 new pacman -Sy calls, got $((N_SY - SY_BEFORE))"; fi
N_BACKUPS="$(backup_count "$CONF4")"
if [[ "$N_BACKUPS" -eq 1 ]]; then pass "B6: exactly one .bak-<epoch> (the append still happened before the failed refresh)"; else fail "B6: expected 1 backup, got $N_BACKUPS"; fi
N_SECTIONS="$(grep -c '^\[cachyos\]$' "$CONF4" || true)"
if [[ "$N_SECTIONS" -eq 1 ]]; then pass "B6: one [cachyos] section appended"; else fail "B6: expected 1 [cachyos] header line, got $N_SECTIONS"; fi

# (v cont.) GPG auto-import on the ALREADY-ENABLED path: the same one-shot
# GPG failure on a re-run (no append, no new backup — only the refresh).
cp "$CONF4" "$KB/conf4.after6"
SY_BEFORE="$(pacman_sy_count)"
KEY_BEFORE="$(grep -c 'pacman-key' "$PACMAN_KEY_LOG" || true)"
export PACMAN_FAIL_GPG="$KB/gpg2"
run_repo_add cachyos --conf "$CONF4"
unset PACMAN_FAIL_GPG
if [[ "$SCRIPT_RC" -eq 0 ]]; then pass "B7: GPG-failure already-enabled run exits 0"; else fail "B7: GPG-failure already-enabled run rc=$SCRIPT_RC: $SCRIPT_OUT"; fi
case "$SCRIPT_OUT" in
    *"already enabled"*) pass "B7: 'already enabled' printed";;
    *) fail "B7: no 'already enabled' in output: $SCRIPT_OUT";;
esac
case "$SCRIPT_OUT" in
    *"Importing GPG key $GPG_ID"*) pass "B7: GPG key import announced on the re-run path too";;
    *) fail "B7: no GPG-key import announcement on the re-run: $SCRIPT_OUT";;
esac
N_SY="$(pacman_sy_count)"
if [[ "$N_SY" -eq $((SY_BEFORE + 2)) ]]; then pass "B7: pacman -Sy called twice (initial + retry) on the re-run"; else fail "B7: expected 2 new pacman -Sy calls, got $((N_SY - SY_BEFORE))"; fi
KEY_TOTAL="$(grep -c 'pacman-key' "$PACMAN_KEY_LOG" || true)"
if [[ "$KEY_TOTAL" -eq $((KEY_BEFORE + 4)) ]]; then pass "B7: four more pacman-key calls (proactive recv+lsign, then reactive recv+lsign)"; else fail "B7: expected 4 new pacman-key calls, got $((KEY_TOTAL - KEY_BEFORE))"; fi
if cmp -s "$CONF4" "$KB/conf4.after6"; then pass "B7: conf byte-identical (no append on the re-run)"; else fail "B7: conf changed on the already-enabled re-run"; fi
N_BACKUPS="$(backup_count "$CONF4")"
if [[ "$N_BACKUPS" -eq 1 ]]; then pass "B7: no new backup (still one)"; else fail "B7: expected 1 backup after the re-run, got $N_BACKUPS"; fi

# (vi) GPG failure with an UNREPAIRABLE key import: every pacman-key call
# fails (the key never lands in the keyring) ⇒ the proactive pair fails
# silently, the reactive --recv-keys fails, its --lsign-key is
# short-circuited by &&, no retry, non-zero exit — the unrepaired error
# surfaces as-is.
CONF5="$KB/conf5"
fresh_conf "$CONF5"
SY_BEFORE="$(pacman_sy_count)"
KEY_BEFORE="$(grep -c 'pacman-key' "$PACMAN_KEY_LOG" || true)"
export PACMAN_FAIL_GPG="$KB/gpg3" PACMAN_KEY_FAIL="$KB/keyfail3"
run_repo_add cachyos --conf "$CONF5"
unset PACMAN_FAIL_GPG PACMAN_KEY_FAIL
if [[ "$SCRIPT_RC" -ne 0 ]]; then pass "B8: unrepaired-import run exits non-zero"; else fail "B8: unrepaired-import run exited 0: $SCRIPT_OUT"; fi
case "$SCRIPT_OUT" in
    *"Importing GPG key $GPG_ID"*) pass "B8: the (failing) import was announced";;
    *) fail "B8: no import announcement: $SCRIPT_OUT";;
esac
case "$SCRIPT_OUT" in
    *"Retrying pacman -Sy"*) fail "B8: a retry happened although every import failed";;
    *) pass "B8: no retry after the failed imports";;
esac
N_SY="$(pacman_sy_count)"
if [[ "$N_SY" -eq $((SY_BEFORE + 1)) ]]; then pass "B8: pacman -Sy called once (no retry)"; else fail "B8: expected 1 new pacman -Sy call, got $((N_SY - SY_BEFORE))"; fi
KEY_TOTAL="$(grep -c 'pacman-key' "$PACMAN_KEY_LOG" || true)"
if [[ "$KEY_TOTAL" -eq $((KEY_BEFORE + 3)) ]]; then pass "B8: three pacman-key calls (proactive recv+lsign, reactive recv; reactive lsign short-circuited)"; else fail "B8: expected 3 new pacman-key calls, got $((KEY_TOTAL - KEY_BEFORE))"; fi
# The log is shared across cases (B6/B7 already appended lines; B9 runs
# after this check), so the per-case pattern is checked on the LAST line:
# it must be the reactive --recv-keys call, proving its --lsign-key was
# short-circuited by &&.
if [[ "$(tail -n1 "$PACMAN_KEY_LOG")" == "pacman-key --recv-keys $GPG_ID" ]]; then pass "B8: the last call is the reactive --recv-keys <id> (--lsign-key short-circuited)"; else fail "B8: unexpected last pacman-key call: $(tail -n1 "$PACMAN_KEY_LOG")"; fi
N_BACKUPS="$(backup_count "$CONF5")"
if [[ "$N_BACKUPS" -eq 1 ]]; then pass "B8: backup + append still happened before the failed refresh"; else fail "B8: expected 1 backup, got $N_BACKUPS"; fi
N_SECTIONS="$(grep -c '^\[cachyos\]$' "$CONF5" || true)"
if [[ "$N_SECTIONS" -eq 1 ]]; then pass "B8: one [cachyos] section appended"; else fail "B8: expected 1 [cachyos] header line, got $N_SECTIONS"; fi

# (vii) UNKNOWN-TRUST repair (the case the user hit live): the key is in
# the keyring but untrusted, so the -Sy error names the signer's uid,
# never a key ID — case 1's parse finds nothing. The script must look the
# key up via `pacman-key --list-keys <uid>`, lsign the found 16-hex
# primary ID, retry, and exit 0.
SIGNER="CachyOS <repo@cachyos.org>"
TRUST_ID="D6C9442437365605"
CONF6="$KB/conf6"
fresh_conf "$CONF6"
SY_BEFORE="$(pacman_sy_count)"
KEY_BEFORE="$(grep -c 'pacman-key' "$PACMAN_KEY_LOG" || true)"
export PACMAN_FAIL_TRUST="$KB/trust1"
run_repo_add cachyos --conf "$CONF6"
unset PACMAN_FAIL_TRUST
if [[ "$SCRIPT_RC" -eq 0 ]]; then pass "B9: unknown-trust run exits 0 (case-2 repair + retry succeeded)"; else fail "B9: unknown-trust run rc=$SCRIPT_RC: $SCRIPT_OUT"; fi
case "$SCRIPT_OUT" in
    *"is unknown trust"*) pass "B9: the unknown-trust error surfaced (then was repaired)";;
    *) fail "B9: no unknown-trust error in output: $SCRIPT_OUT";;
esac
case "$SCRIPT_OUT" in
    *"Signing untrusted key $TRUST_ID"*) pass "B9: the looked-up key ID was signed (announced)";;
    *) fail "B9: no key-sign announcement: $SCRIPT_OUT";;
esac
if grep -qx "pacman-key --list-keys $SIGNER" "$PACMAN_KEY_LOG"; then pass "B9: the key was looked up by the signer's uid"; else fail "B9: pacman-key --list-keys <signer> missing"; fi
if grep -qx "pacman-key --lsign-key $TRUST_ID" "$PACMAN_KEY_LOG"; then pass "B9: the looked-up 16-hex ID was locally signed"; else fail "B9: pacman-key --lsign-key <id> missing"; fi
case "$SCRIPT_OUT" in
    *"Retrying pacman -Sy"*) pass "B9: the retry was announced";;
    *) fail "B9: no retry announcement: $SCRIPT_OUT";;
esac
N_SY="$(pacman_sy_count)"
if [[ "$N_SY" -eq $((SY_BEFORE + 2)) ]]; then pass "B9: pacman -Sy called twice (initial + retry)"; else fail "B9: expected 2 new pacman -Sy calls, got $((N_SY - SY_BEFORE))"; fi
KEY_TOTAL="$(grep -c 'pacman-key' "$PACMAN_KEY_LOG" || true)"
if [[ "$KEY_TOTAL" -eq $((KEY_BEFORE + 4)) ]]; then pass "B9: four pacman-key calls (proactive recv+lsign, then case-2 list-keys+lsign)"; else fail "B9: expected 4 new pacman-key calls, got $((KEY_TOTAL - KEY_BEFORE))"; fi
N_BACKUPS="$(backup_count "$CONF6")"
if [[ "$N_BACKUPS" -eq 1 ]]; then pass "B9: backup + append still happened before the failed refresh"; else fail "B9: expected 1 backup, got $N_BACKUPS"; fi
N_SECTIONS="$(grep -c '^\[cachyos\]$' "$CONF6" || true)"
if [[ "$N_SECTIONS" -eq 1 ]]; then pass "B9: one [cachyos] section appended"; else fail "B9: expected 1 [cachyos] header line, got $N_SECTIONS"; fi

# The live /etc/pacman.conf must be byte-identical to the pre-run state.
ETC_MD5_AFTER="$(md5sum /etc/pacman.conf 2>/dev/null | awk '{print $1}' || true)"
if [[ -n "$ETC_MD5_BEFORE" ]]; then
    if [[ "$ETC_MD5_BEFORE" == "$ETC_MD5_AFTER" ]]; then pass "B5: live /etc/pacman.conf untouched (md5 identical)"; else fail "B5: live /etc/pacman.conf CHANGED"; fi
else
    echo "INFO: /etc/pacman.conf not readable — md5 gate skipped (writes are sandboxed by construction)"
fi

if [[ "$FAILURES" -eq 0 ]]; then
    echo "K13-BASH: all assertions passed"
else
    echo "K13-BASH: $FAILURES failure(s)" >&2
    exit 1
fi
echo "K13: OK (part A green + part B green)"
