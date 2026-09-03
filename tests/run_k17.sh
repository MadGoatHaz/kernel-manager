#!/usr/bin/env bash
# k17: harness for src/terminal-helper — the SENTINEL launch/rc protocol
# (chunk D, closes H3). Bash-only, no C++ (K8/k13B pattern): an mktemp
# sandbox with FAKE terminals on PATH and a whitelisted coreutils dir
# (bash, mktemp, sleep, cat, rm, touch, chmod), so the helper's terminal
# selection is deterministic regardless of what the host has installed.
# The helper is invoked exactly as the C++ invokes it (run_cmd_async /
# runCmdTerminal) in the sandbox CWD:
#
#     terminal-helper [-s 'pkexec <rootshell>'] '<cmd>; read -p "Press enter to exit"'
#
# Contract (chunk D): the command runs inside a wrapper that drops
# filesystem sentinels (started / rc / done) in a world-readable run dir;
# the helper polls them and EXITS WITH THE COMMAND'S REAL RC — for every
# launch (blocking or non-blocking, escalated or not). The old launch
# contract (non-blocking ⇒ helper rc 0) and the old escalated
# fire-and-forget (⇒ helper rc 0 immediately) are gone; the launch-
# contract cases below are re-specified to the mapped wrapper rc:
#   started never appears ⇒ helper 126 (Polkit rejected / never launched)
#   done never appears (30 min) ⇒ helper 124 (command timeout)
#   otherwise ⇒ helper = the command's real rc from the rc sentinel
# (the INT/TERM traps report 130/143; a command-level `exit` kills only
# the wrapper's subshell, so the rc bookkeeping always runs).
#
# Fakes: konsole = D-Bus mimicry (background-spawn the "window", record the
# child PID, exit 0 immediately — the window's output goes to /dev/null, not
# back down the launcher's stdout); xterm = blocking (foreground exec, exit
# with its rc); pkexec = run the program directly (no privilege transition);
# pkexec-rejected = Polkit denial (exit 1, never runs the program).
# Cases: (1) konsole + `sleep 1; read` (Enter fed via fifo) => helper rc 0
# (read's rc), waited >= 1 s, cleaned; (2) konsole + `sleep 30`, window
# shell TERM'd after ~1 s => helper rc 143 (the TERM trap) within ~5 s,
# cleaned; (3) xterm + `exit 7` => helper rc 7 (subshell isolation), no
# grace hang (< 1 s); (4) escalated konsole + `sleep 1; read` (EOF stdin)
# => helper rc 1 (read's real rc — the H3 fix: no fire-and-forget 0) after
# ~1 s; (5) konsole + `exit 5` => helper rc 5 (the command rc, not the old
# launch contract 0); (6) konsole + "cd '<sub>' && touch made-it; ..." =>
# rc 1 + the marker lands in the cd'd subdir (the run_cmd_async CWD-prefix
# shape works through the wrapper; the wrapper itself never cds, so the
# command's own cd still resolves against the terminal's launch CWD).
# (7) ESCALATED konsole + `exit 7` => helper rc 7 — the H3 case: the old
# fire-and-forget path reported 0 here; (8) ESCALATED konsole + Polkit
# REJECTED (the fake pkexec never runs the rootshell) => helper rc 126
# (started never seen) within the KMH_START_TIMEOUT override; (9) konsole
# + `sleep 1; exit 3` => helper rc 3 after the wrapper wrote rc+done;
# (10) konsole + `sleep 30` (a >30-min command stand-in) => helper rc 124
# (done never seen) via the KMH_DONE_TIMEOUT override; (11) no temp leak:
# the sandbox TMPDIR (run dir + wrapper file + pidfile) is empty.
#
# The KMH_START_TIMEOUT / KMH_DONE_TIMEOUT env overrides (production
# defaults 120 s / 1800 s) exist for the test harness only — cases 8/10
# use 5 s deadlines (wide bounds: this box's bash SECONDS jitters by up to
# ~0.6 s per read) so the suite runs in seconds, not 32 minutes.
set -euo pipefail
cd "$(dirname "$0")/.."

ROOT="$(pwd)"
HELPER="$ROOT/src/terminal-helper"

PASS=0
FAIL=0
ok()  { PASS=$((PASS + 1)); printf 'PASS  %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf 'FAIL  %s\n' "$1"; }
check_eq() {
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (actual: $2, expected: $3)"; fi
}
check_ge() {
    if [ "$2" -ge "$3" ]; then ok "$1"; else bad "$1 (actual: $2, bound: $3)"; fi
}
check_lt() {
    if [ "$2" -lt "$3" ]; then ok "$1"; else bad "$1 (actual: $2, bound: $3)"; fi
}
check_gone() {
    if [ ! -e "$2" ]; then ok "$1"; else bad "$1 (still present: $2)"; fi
}

# --- sandbox ---------------------------------------------------------------
SBX="$(mktemp -d)"
trap 'rm -rf "$SBX"' EXIT
BIN="$SBX/bin"
ESSENTIAL="$SBX/essential"
mkdir -p "$BIN" "$ESSENTIAL" "$SBX/tmp"

# Whitelisted coreutils the helper + fakes may use under the sandbox PATH.
# No host terminal (ptyxis/konsole/xterm/...) is reachable through it, so
# the helper can only ever pick one of the fakes. chmod is needed by the
# helper's sentinel run-dir setup (the chunk D addition).
for tool in bash mktemp sleep cat rm touch chmod; do
    ln -s "$(command -v "$tool")" "$ESSENTIAL/$tool"
done

# The helper's first stdout line is the unchanged `echo $cmd` debug line
# ('<launcher> "<file>"') — strip the quotes, take the last word = the file.
extract_file() {
    printf '%s\n' "$OUT" | head -1 | tr -d '"' | awk '{print $NF}'
}

# Invoke the helper exactly as the C++ does (sandbox CWD, sandbox PATH,
# sandbox TMPDIR so every temp artifact the helper creates lands where
# case 11 can assert no leak). $1 = stdin source; remaining args = the
# helper's args. $HEXTRA carries optional env assignments (the KMH_* test
# deadline overrides). Sets RC (exit code), OUT (stdout), MS (elapsed
# milliseconds).
HEXTRA=""
invoke_helper() {
    local stdin_src="$1"; shift
    local t0 t1
    t0=$(date +%s%3N)
    RC=0
    # env: $HEXTRA is an unquoted expansion (KMH_* test deadline overrides) —
    # bash would not re-parse the expanded words as assignments, so route
    # them through env (empty $HEXTRA = plain env, no assignments).
    OUT=$(cd "$SBX" && env PATH="$BIN:$ESSENTIAL" TMPDIR="$SBX/tmp" K17_PIDFILE="$K17_PIDFILE" $HEXTRA "$HELPER" "$@" < "$stdin_src") || RC=$?
    t1=$(date +%s%3N)
    MS=$(( t1 - t0 ))
}

# --- fakes (rebuilt per case) ----------------------------------------------
reset_bin() {
    rm -rf "$BIN"
    mkdir -p "$BIN"
}
make_fake_konsole() {
    cat > "$BIN/konsole" <<'EOF'
#!/bin/bash
# k17 fake: D-Bus mimicry — background-spawn the "window" (its output goes
# to /dev/null, NOT back down the launcher's stdout) and exit 0 immediately.
# set -m: POSIX gives a `&` job in a non-interactive, job-control-less shell
# its stdin from /dev/null, which would make any interactive `read` in the
# command EOF instantly; with job control the job inherits the fake's stdin
# (= the helper's stdin), so a fifo-fed read behaves like the real window's
# terminal.
set -m
shift # drop -e
"$@" > /dev/null 2>&1 &
child=$!
if [ -n "${K17_PIDFILE:-}" ]; then
    echo "$child" > "$K17_PIDFILE"
fi
exit 0
EOF
    chmod +x "$BIN/konsole"
}
make_fake_xterm() {
    cat > "$BIN/xterm" <<'EOF'
#!/bin/bash
# k17 fake: blocking — foreground exec of the requested command, exit with its rc.
shift # drop -e
exec "$@"
EOF
    chmod +x "$BIN/xterm"
}
make_fake_pkexec() {
    cat > "$BIN/pkexec" <<'EOF'
#!/bin/bash
# k17 fake: run the requested program directly (no privilege transition).
exec "$@"
EOF
    chmod +x "$BIN/pkexec"
}
make_fake_pkexec_rejected() {
    cat > "$BIN/pkexec" <<'EOF'
#!/bin/bash
# k17 fake: Polkit rejection — exit 1 WITHOUT running the program (the
# escalated wrapper therefore never runs: no started sentinel appears).
exit 1
EOF
    chmod +x "$BIN/pkexec"
}
make_rootshell() {
    cat > "$SBX/rootshell.sh" <<'EOF'
#!/bin/bash
# k17 sandbox "root shell" (mimics the installed KM_HELPER_DIR/rootshell.sh).
exec bash "$1"
EOF
    chmod +x "$SBX/rootshell.sh"
}

# --- gate: syntax ------------------------------------------------------------
rc=0
bash -n "$HELPER" || rc=$?
check_eq 'gate: bash -n src/terminal-helper (syntax)' "$rc" 0

# case 1: non-blocking (konsole) + sleep 1; the user presses Enter (fed via
# the fifo) after the command => the sentinel's rc is read's rc = 0
echo '== case 1: non-blocking (konsole) + sleep 1 — the helper must wait, rc 0 =='
reset_bin; make_fake_konsole
K17_PIDFILE="$SBX/kc1"; rm -f "$K17_PIDFILE"
HEXTRA=""
mkfifo "$SBX/fifo1"
# Simulate the user pressing enter after the command finishes: feed one
# newline down the pipe the window's shell `read`s from at ~1.5 s.
( sleep 1.5; printf '\n'; sleep 20 ) > "$SBX/fifo1" &
feeder=$!
invoke_helper "$SBX/fifo1" 'sleep 1; read -p "Press enter to exit"'
kill "$feeder" 2>/dev/null || true
wait "$feeder" 2>/dev/null || true
file="$(extract_file)"
check_eq   'case 1: helper rc = 0 (sentinel rc: the read succeeded)' "$RC" 0
check_ge   "case 1: helper waited for the command (elapsed ${MS}ms >= 1000ms)" "$MS" 1000
check_gone 'case 1: wrapper file cleaned (helper safety net)' "$file"
check_gone 'case 1: pid marker cleaned' "$file.pid"
check_eq   'case 1: fake konsole recorded the window child (D-Bus path used)' \
    "$([ -s "$K17_PIDFILE" ] && echo yes || no)" yes

# case 2: konsole + sleep 30, window's shell TERM'd after ~1 s — the TERM
# trap writes rc 143 (re-specified from the old launch-contract rc 0)
echo '== case 2: non-blocking (konsole) + sleep 30, window close — TERM trap rc 143 =='
reset_bin; make_fake_konsole
K17_PIDFILE="$SBX/kc2"; rm -f "$K17_PIDFILE"
HEXTRA=""
t0=$(date +%s%3N)
( cd "$SBX" && env PATH="$BIN:$ESSENTIAL" TMPDIR="$SBX/tmp" K17_PIDFILE="$K17_PIDFILE" $HEXTRA \
    "$HELPER" 'sleep 30; read -p "Press enter to exit"' > "$SBX/out2" < /dev/null ) &
hp=$!
sleep 1.2
child=$(cat "$K17_PIDFILE" 2>/dev/null || true)
pkill -TERM -P "$child" 2>/dev/null || true   # the wrapper's subshell
kill -TERM "$child" 2>/dev/null || true       # the wrapper shell (TERM trap)
RC=0; wait "$hp" || RC=$?
t1=$(date +%s%3N); MS=$(( t1 - t0 ))
OUT=$(cat "$SBX/out2")
file="$(extract_file)"
check_eq   'case 2: helper rc = 143 (the SIGTERM trap — re-specified from launch-contract 0)' "$RC" 143
check_lt   "case 2: helper terminated after the window closed (elapsed ${MS}ms < 5000ms)" "$MS" 5000
check_gone 'case 2: wrapper file removed by the helper' "$file"
check_gone 'case 2: pid marker removed by the helper' "$file.pid"

# case 3: blocking (xterm) + command rc 7 — the `exit 7` kills only the
# wrapper's subshell, so the rc/done bookkeeping still runs (rc 7)
echo '== case 3: blocking (xterm) + command rc 7 — rc propagation, no grace hang =='
reset_bin; make_fake_xterm
K17_PIDFILE=""
HEXTRA=""
invoke_helper /dev/null 'exit 7; read -p "Press enter to exit"'
file="$(extract_file)"
check_eq   'case 3: helper rc = 7 (command rc propagated)' "$RC" 7
check_lt   "case 3: no grace hang (elapsed ${MS}ms < 1000ms)" "$MS" 1000
check_gone 'case 3: wrapper file removed (the subshell exit stopped it before self-cleanup — helper net)' "$file"
check_gone 'case 3: pid marker removed' "$file.pid"

# case 4: escalated (pkexec rootshell) + non-blocking (konsole) — re-
# specified from the old fire-and-forget rc 0: the helper now waits for
# the sentinel and returns the REAL rc (read on EOF stdin = 1)
echo '== case 4: escalated (pkexec rootshell) + non-blocking (konsole) — real rc, not fire-and-forget =='
reset_bin; make_fake_konsole; make_fake_pkexec; make_rootshell
K17_PIDFILE="$SBX/kc4"; rm -f "$K17_PIDFILE"
HEXTRA=""
invoke_helper /dev/null -s "pkexec $SBX/rootshell.sh" 'sleep 1; read -p "Press enter to exit"'
file="$(extract_file)"
check_eq   'case 4: helper rc = 1 (the real rc of the EOF read — the H3 fix: no fire-and-forget 0)' "$RC" 1
check_ge   "case 4: helper waited for the sentinel (elapsed ${MS}ms >= 1000ms)" "$MS" 1000
check_lt   "case 4: helper returned after the command ended (elapsed ${MS}ms < 5000ms)" "$MS" 5000
check_gone 'case 4: wrapper file cleaned by the helper' "$file"
check_eq   'case 4: fake konsole recorded the window child (D-Bus path used)' \
    "$([ -s "$K17_PIDFILE" ] && echo yes || no)" yes

# case 5: non-blocking (konsole) + failing command — re-specified from the
# old launch-contract rc 0 to the command's real rc 5 (the subshell dies,
# the wrapper records it)
echo '== case 5: non-blocking (konsole) + failing command (exit 5) — real rc 5 =='
reset_bin; make_fake_konsole
K17_PIDFILE="$SBX/kc5"; rm -f "$K17_PIDFILE"
HEXTRA=""
invoke_helper /dev/null 'exit 5; read -p "Press enter to exit"'
file="$(extract_file)"
check_eq   'case 5: helper rc = 5 (the command rc — re-specified from launch-contract 0)' "$RC" 5
check_lt   "case 5: helper exited promptly after the shell died (elapsed ${MS}ms < 3000ms)" "$MS" 3000
check_gone 'case 5: wrapper file cleaned by the helper' "$file"
check_gone 'case 5: pid marker cleaned by the helper' "$file.pid"

# case 6: konsole + the run_cmd_async CWD prefix — "cd '<dir>' && cmd"
# must run in the terminal's launch CWD (the wrapper itself never cds),
# and the rc is read's EOF rc 1 (re-specified from the old launch-contract 0)
echo '== case 6: non-blocking (konsole) + cd prefix — marker lands in the cd dir, rc 1 =='
reset_bin; make_fake_konsole
K17_PIDFILE="$SBX/kc6"; rm -f "$K17_PIDFILE"
HEXTRA=""
mkdir -p "$SBX/sub"
# The sleep 1 models a command of finite duration; the trailing read on
# EOF stdin (the app's real commands end in an interactive `read`) is rc 1.
CMD6="cd '$SBX/sub' && touch made-it; sleep 1; read -p 'Press enter to exit'"
invoke_helper /dev/null "$CMD6"
file="$(extract_file)"
check_eq   'case 6: helper rc = 1 (read EOF — re-specified from launch-contract 0)' "$RC" 1
check_ge   "case 6: helper waited for the command (elapsed ${MS}ms >= 1000ms)" "$MS" 1000
check_gone 'case 6: wrapper file cleaned' "$file"
check_gone 'case 6: pid marker cleaned' "$file.pid"
check_eq   "case 6: marker landed in the cd'd subdir" "$([ -f "$SBX/sub/made-it" ] && echo yes || echo no)" yes
check_eq   'case 6: no marker in the helper launch CWD' "$([ -f "$SBX/made-it" ] && echo yes || echo no)" no

# case 7: the H3 case — ESCALATED non-blocking konsole + a failing command
# (the old fire-and-forget path reported 0 here; the sentinel returns 7)
echo '== case 7: escalated (pkexec rootshell) + konsole + exit 7 — the H3 case, rc captured =='
reset_bin; make_fake_konsole; make_fake_pkexec; make_rootshell
K17_PIDFILE="$SBX/kc7"; rm -f "$K17_PIDFILE"
HEXTRA=""
invoke_helper /dev/null -s "pkexec $SBX/rootshell.sh" 'exit 7'
file="$(extract_file)"
check_eq   'case 7: helper rc = 7 (the escalated command rc — H3 closed)' "$RC" 7
check_lt   "case 7: no launch grace hang (elapsed ${MS}ms < 3000ms)" "$MS" 3000
check_gone 'case 7: wrapper file cleaned by the helper' "$file"
check_eq   'case 7: fake konsole recorded the window child' \
    "$([ -s "$K17_PIDFILE" ] && echo yes || no)" yes

# case 8: escalated + Polkit REJECTED — the fake pkexec never runs the
# rootshell ⇒ the wrapper never runs ⇒ `started` never appears ⇒ 126.
# KMH_START_TIMEOUT=5 keeps the suite in seconds (production default 120);
# the bounds are wide because this box's bash SECONDS (the deadline's clock)
# jitters by up to ~0.6 s per read — the production 120 s deadline is
# unaffected at that scale (a "no wait" failure exits in < 0.5 s, far below
# the 4000 ms lower bound).
echo '== case 8: escalated + pkexec rejected (Polkit denied) — started never seen, rc 126 =='
reset_bin; make_fake_konsole; make_fake_pkexec_rejected; make_rootshell
K17_PIDFILE="$SBX/kc8"; rm -f "$K17_PIDFILE"
HEXTRA="KMH_START_TIMEOUT=5"
invoke_helper /dev/null -s "pkexec $SBX/rootshell.sh" 'exit 7'
file="$(extract_file)"
check_eq   'case 8: helper rc = 126 (Polkit rejected / window never opened)' "$RC" 126
check_ge   "case 8: helper waited the start deadline (elapsed ${MS}ms >= 4000ms)" "$MS" 4000
check_lt   "case 8: helper bounded by the deadline (elapsed ${MS}ms < 8000ms)" "$MS" 8000
check_gone 'case 8: wrapper file cleaned on the 126 path' "$file"
HEXTRA=""

# case 9: non-escalated konsole + `sleep 1; exit 3` — the subshell exits 3,
# the wrapper writes rc+done, the helper returns 3
echo '== case 9: non-blocking (konsole) + sleep 1; exit 3 — rc 3 after rc+done =='
reset_bin; make_fake_konsole
K17_PIDFILE="$SBX/kc9"; rm -f "$K17_PIDFILE"
HEXTRA=""
invoke_helper /dev/null 'sleep 1; exit 3'
file="$(extract_file)"
check_eq   'case 9: helper rc = 3 (the sentinel rc after done)' "$RC" 3
check_ge   "case 9: helper waited for the command (elapsed ${MS}ms >= 1000ms)" "$MS" 1000
check_lt   "case 9: helper returned after done (elapsed ${MS}ms < 5000ms)" "$MS" 5000
check_gone 'case 9: wrapper file cleaned by the helper' "$file"

# case 10: a command running past the 30-min limit ⇒ 124. `sleep 30` is the
# stand-in; KMH_DONE_TIMEOUT=5 keeps the suite in seconds (production
# default 1800). The started sentinel IS seen (the wrapper ran); only done
# is missing when the deadline hits. Same wide bounds as case 8 (SECONDS
# jitter on this box).
echo '== case 10: konsole + sleep 30 with a short done deadline — command timeout rc 124 =='
reset_bin; make_fake_konsole
K17_PIDFILE="$SBX/kc10"; rm -f "$K17_PIDFILE"
HEXTRA="KMH_DONE_TIMEOUT=5"
invoke_helper /dev/null 'sleep 30'
file="$(extract_file)"
check_eq   'case 10: helper rc = 124 (30-min command timeout)' "$RC" 124
check_ge   "case 10: helper waited the done deadline (elapsed ${MS}ms >= 4000ms)" "$MS" 4000
check_lt   "case 10: helper bounded by the deadline (elapsed ${MS}ms < 8000ms)" "$MS" 8000
check_gone 'case 10: wrapper file cleaned on the 124 path' "$file"
HEXTRA=""

# case 11: no temp leak — the helper's run dir, wrapper file, and pidfile
# all land in the sandbox TMPDIR and must be gone after every exit path
# (126 / 124 / real rc). The case 10 orphan (sleep 30) may still be alive
# here, but its writes target the already-removed run dir, so it cannot
# recreate anything.
echo '== case 11: no temp leak (sandbox TMPDIR empty after all cases) =='
leftover="$(ls -A "$SBX/tmp" 2>/dev/null | wc -l | tr -d ' ')"
check_eq 'case 11: run dir + wrapper file + pidfile all cleaned (no /tmp leak)' "$leftover" 0

# --- summary-------------------------------------------------------------------
echo
echo "k17: ${PASS} passed, ${FAIL} failed"
if [ "$FAIL" -ne 0 ]; then
    exit 1
fi
