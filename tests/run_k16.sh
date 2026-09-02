#!/usr/bin/env bash
# k16: build_helper.sh GPG three-needle extraction (D4) — bash-only.
#
# Design: a mktemp sandbox holds a stateful makepkg stub (mode file
# first-fail-<case>: the first call fails with the case's real failure text,
# later calls succeed; every call is logged) and a gpg stub (logs the
# --recv-keys / --import-ownertrust arguments, always exits 0). The stubs
# shadow the real makepkg / gpg via PATH, so zero system state is written —
# the real binaries are never touched (k13B sandbox-PATH pattern). The
# makepkg stub ignores the --noconfirm flag the helper appends; its stdout
# is what the helper tees into the log file.
#
# Streaming: the helper tees makepkg's output to the terminal AND to a log
# file under ${XDG_CACHE_HOME}/kernel-manager/build-<ts>-<pid>.log. The
# sandbox points XDG_CACHE_HOME at its own cache dir (hermetic), and each
# case verifies the needle landed in that log file.
#
# Gates:
#   0.  bash -n src/build_helper.sh (syntax)
#   A.  "unknown public key <40-hex>" (libalpm) — v1.23.0 regression guard:
#       1 recv-keys (exact 40-hex) + 1 ownertrust + exactly 2 makepkg calls
#       (both with --noconfirm) + the needle in the log file + rc 0
#   B.  "key <16-hex>: … is not in your keyring" (gpg) — the 16-hex is
#       extracted from the line, retry, rc 0
#   C.  "Can't check signature: No public key" + paired "using RSA key ID
#       <16-hex>" (gpg) — the 16-hex is extracted from the paired line,
#       retry, rc 0
#   C2. "No public key" with NO key-ID line — the manual hint is printed,
#       zero gpg calls, exactly one retry, rc = the retry's rc (0: the stub
#       succeeds on the retry)
#   D.  a non-GPG failure — exactly 1 makepkg call, zero gpg calls, rc 1
#       (no blind retry), the failure is logged
set -euo pipefail
cd "$(dirname "$0")/.."

ROOT="$(pwd)"
HELPER="$ROOT/src/build_helper.sh"
SANDBOX="$(mktemp -d /tmp/km16-sandbox.XXXXXX)"
BIN="$SANDBOX/bin"
CACHE="$SANDBOX/cache"
trap 'rm -rf "$SANDBOX"' EXIT

PASS=0
FAIL=0
check() { # check <name> <command...> — passes when the command exits 0
    local name="$1"
    shift
    if "$@"; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        echo "FAIL: $name"
    fi
}
check_absent() { # check_absent <name> <command...> — passes when the command finds nothing
    local name="$1"
    shift
    if "$@"; then
        FAIL=$((FAIL + 1))
        echo "FAIL: $name"
    else
        PASS=$((PASS + 1))
    fi
}
count_lines() { # count_lines <file> — line count, 0 when the file is absent
    if [[ -f "$1" ]]; then
        wc -l < "$1"
    else
        echo 0
    fi
}

# --- gate 0: syntax ---------------------------------------------------------
check "gate0: bash -n src/build_helper.sh" bash -n "$HELPER"

# --- the stateful makepkg stub (first-fail-<case> then succeed; logs calls) -
# The helper appends --noconfirm to the args; the stub ignores all of them.
# Its stdout is what the helper streams through tee into the log file.
mkdir -p "$BIN"
cat > "$BIN/makepkg" <<'EOF'
#!/usr/bin/env bash
LOG="$K16_SANDBOX/makepkg_calls.log"
echo "call: $*" >> "$LOG"
if [[ -f "$K16_SANDBOX/makepkg_mode" ]]; then
    mode="$(cat "$K16_SANDBOX/makepkg_mode")"
    rm -f "$K16_SANDBOX/makepkg_mode"
    if [[ "$mode" == first-fail-* ]]; then
        cat "$K16_SANDBOX/fail_text"
        exit 1
    fi
fi
echo "stub: package built successfully"
exit 0
EOF

# --- the gpg stub (logs the --recv-keys / --import-ownertrust args, exit 0) -
cat > "$BIN/gpg" <<'EOF'
#!/usr/bin/env bash
LOG="$K16_SANDBOX/gpg_calls.log"
if [[ "${1:-}" == "--recv-keys" ]]; then
    echo "recv-keys $2" >> "$LOG"
elif [[ "${1:-}" == "--import-ownertrust" ]]; then
    echo "ownertrust $(cat)" >> "$LOG"
else
    echo "unrecognized: $*" >> "$LOG"
fi
exit 0
EOF
chmod +x "$BIN/makepkg" "$BIN/gpg"

# --- the case runner: resets per-case state, drives the real helper --------
# XDG_CACHE_HOME points into the sandbox so the helper's log file is
# hermetic; $$ in the log name makes each run's file unique.
run_case() { # run_case <id> <fail-text line...>
    local id="$1"
    shift
    rm -f "$SANDBOX/makepkg_mode" "$SANDBOX/makepkg_calls.log" \
          "$SANDBOX/gpg_calls.log" "$SANDBOX/fail_text" "$SANDBOX/out.log"
    rm -rf "$CACHE"
    mkdir -p "$CACHE"
    printf '%s\n' "$@" > "$SANDBOX/fail_text"
    printf 'first-fail-%s\n' "$id" > "$SANDBOX/makepkg_mode"
    set +e
    PATH="$BIN:$PATH" K16_SANDBOX="$SANDBOX" XDG_CACHE_HOME="$CACHE" \
        bash "$HELPER" -sfC "$SANDBOX/pkg" \
        > "$SANDBOX/out.log" 2>&1
    RC=$?
    set -e
    K16_LOG="$(find "$CACHE/kernel-manager" -name 'build-*.log' 2>/dev/null | head -n1)"
}

# --- case A: libalpm "unknown public key <40-hex>" (v1.23.0 regression) ----
echo "INFO: case A — unknown public key <40-hex>"
run_case A \
    "==> Checking dependencies..." \
    "error: failed to retrieve 'kernel-manager' from any mirror" \
    "warning: unknown public key 882DCFE48E2562ABF3B607488DB35A47" \
    "==> ERROR: Could not sync"
check "A: rc 0 (the retry succeeded)" test "$RC" -eq 0
check "A: exactly 2 makepkg calls" test "$(count_lines "$SANDBOX/makepkg_calls.log")" -eq 2
check "A: makepkg called with --noconfirm" grep -q -- '--noconfirm' "$SANDBOX/makepkg_calls.log"
check "A: exactly 2 gpg calls" test "$(count_lines "$SANDBOX/gpg_calls.log")" -eq 2
check "A: recv-keys of the exact 40-hex" grep -qx 'recv-keys 882DCFE48E2562ABF3B607488DB35A47' "$SANDBOX/gpg_calls.log"
check "A: ownertrust '<id>:5' for it" grep -qx 'ownertrust 882DCFE48E2562ABF3B607488DB35A47:5' "$SANDBOX/gpg_calls.log"
check "A: log file exists" test -n "$K16_LOG"
check "A: log file captured the needle" grep -q 'unknown public key 882DCFE48E2562ABF3B607488DB35A47' "$K16_LOG"

# --- case B: gpg "is not in your keyring" (the 16-hex on the line) --------
echo "INFO: case B — key … is not in your keyring"
run_case B \
    'gpg: key C5ADB4F3FEBBCE27: "Kernel Manager Dev <dev@kmanager.example>" is not in your keyring'
check "B: rc 0 (the retry succeeded)" test "$RC" -eq 0
check "B: exactly 2 makepkg calls" test "$(count_lines "$SANDBOX/makepkg_calls.log")" -eq 2
check "B: exactly 2 gpg calls" test "$(count_lines "$SANDBOX/gpg_calls.log")" -eq 2
check "B: recv-keys of the 16-hex from the line" grep -qx 'recv-keys C5ADB4F3FEBBCE27' "$SANDBOX/gpg_calls.log"
check "B: ownertrust '<id>:5' for it" grep -qx 'ownertrust C5ADB4F3FEBBCE27:5' "$SANDBOX/gpg_calls.log"
check "B: log file exists" test -n "$K16_LOG"
check "B: log file captured the needle" grep -q 'is not in your keyring' "$K16_LOG"

# --- case C: gpg "No public key" + the paired "using RSA key ID" line -----
echo "INFO: case C — Can't check signature: No public key"
run_case C \
    "gpg: Can't check signature: No public key" \
    "gpg: Signature made Tue 01 Sep 2026 12:00:00 PM UTC using RSA key ID F3B607488DB35A47"
check "C: rc 0 (the retry succeeded)" test "$RC" -eq 0
check "C: exactly 2 makepkg calls" test "$(count_lines "$SANDBOX/makepkg_calls.log")" -eq 2
check "C: exactly 2 gpg calls" test "$(count_lines "$SANDBOX/gpg_calls.log")" -eq 2
check "C: recv-keys of the 16-hex from the paired line" grep -qx 'recv-keys F3B607488DB35A47' "$SANDBOX/gpg_calls.log"
check "C: ownertrust '<id>:5' for it" grep -qx 'ownertrust F3B607488DB35A47:5' "$SANDBOX/gpg_calls.log"
check "C: log file exists" test -n "$K16_LOG"
check "C: log file captured the paired key-ID line" grep -q 'using RSA key ID F3B607488DB35A47' "$K16_LOG"

# --- case C2: "No public key" with NO key-ID line → hint + one retry ------
echo "INFO: case C2 — No public key, no key ID"
run_case C2 \
    "gpg: Can't check signature: No public key"
check "C2: rc = the retry's rc (0: the stub succeeds)" test "$RC" -eq 0
check "C2: exactly 2 makepkg calls (still one retry)" test "$(count_lines "$SANDBOX/makepkg_calls.log")" -eq 2
check "C2: the manual hint is printed" grep -qF 'gpg --recv-keys <id>' "$SANDBOX/out.log"
check "C2: zero gpg calls (no import)" test "$(count_lines "$SANDBOX/gpg_calls.log")" -eq 0
check "C2: log file exists" test -n "$K16_LOG"
check "C2: log file captured the needle" grep -q 'No public key' "$K16_LOG"

# --- case D: a non-GPG failure → no import, no retry, rc 1 ----------------
echo "INFO: case D — non-GPG failure"
run_case D \
    "==> Entering fakeroot environment..." \
    "make: *** [Makefile:42: build] Error 1" \
    "==> ERROR: Build failed"
check "D: rc 1 (the original failure propagates)" test "$RC" -eq 1
check "D: exactly 1 makepkg call (no blind retry)" test "$(count_lines "$SANDBOX/makepkg_calls.log")" -eq 1
check "D: zero gpg calls" test "$(count_lines "$SANDBOX/gpg_calls.log")" -eq 0
check_absent "D: no manual hint on a non-GPG failure" grep -qF 'gpg --recv-keys <id>' "$SANDBOX/out.log"
check "D: log file exists" test -n "$K16_LOG"
check "D: log file captured the failure" grep -q 'Build failed' "$K16_LOG"

echo ""
echo "K16: $PASS passed, $FAIL failed"
if [[ "$FAIL" -ne 0 ]]; then
    echo "error: k16 failed" >&2
    exit 1
fi
echo "K16: all checks PASS (sandbox removed by the trap)"
