#!/usr/bin/env bash
# k17: harness for C2 (src/terminal-helper — the konsole D-Bus race + the
# launch contract). Bash-only, no C++ (K8/k13B pattern): an mktemp sandbox
# with FAKE terminals on PATH and a whitelisted coreutils dir (bash, mktemp,
# sleep, cat, rm), so the helper's terminal selection is deterministic
# regardless of what the host has installed. The helper is invoked exactly
# as the C++ invokes it (run_cmd_async / runCmdTerminal) in the sandbox CWD:
#
#     terminal-helper [-s 'pkexec <rootshell>'] '<cmd>; read -p "Press enter to exit"'
#
# run_cmd_async additionally prepends "cd '<working_path>' && " to the
# command (CWD safety: a D-Bus terminal runs in its own profile CWD, not
# the QProcess CWD); case 6 exercises exactly that shape.
#
# Fakes: konsole = D-Bus mimicry (background-spawn the "window", record the
# child PID, exit 0 immediately — the window's output goes to /dev/null, not
# back down the launcher's stdout); xterm = blocking (foreground exec, exit
# with its rc); pkexec = run the program directly (no privilege transition).
# Cases: (1) konsole + `sleep 1` => the helper WAITED (>= 1 s), rc 0, file +
# pidfile gone (self-cleaned); (2) konsole + `sleep 30`, window's shell
# killed after 1 s => helper exits within ~5 s (safety net), rc 0, file
# gone; (3) xterm + rc 7 => helper rc 7 (rc propagation), no grace hang
# (< 1 s); (4) escalated + konsole => helper exits 0 immediately (< 1 s,
# fire-and-forget), file self-removed by the "root" shell within 2 s;
# (5) konsole + `exit 5` => helper rc 0 (launch contract — the command's
# rc is the terminal's business, not the helper's); (6) konsole +
# "cd '<sub>' && touch made-it; sleep 1" => the marker lands in the cd'd
# subdir (the run_cmd_async CWD-prefix shape works through the temp file;
# the sleep keeps the PID marker alive past the helper's 0.2 s grace tick
# — an instant command with EOF stdin would race the grace loop, which the
# app's real commands never do: they end in an interactive `read`).
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
mkdir -p "$BIN" "$ESSENTIAL"

# Whitelisted coreutils the helper + fakes may use under the sandbox PATH.
# No host terminal (ptyxis/konsole/xterm/...) is reachable through it, so
# the helper can only ever pick one of the fakes.
for tool in bash mktemp sleep cat rm touch; do
    ln -s "$(command -v "$tool")" "$ESSENTIAL/$tool"
done

# The helper's first stdout line is the unchanged `echo $cmd` debug line
# ('<launcher> "<file>"') — strip the quotes, take the last word = the file.
extract_file() {
    printf '%s\n' "$OUT" | head -1 | tr -d '"' | awk '{print $NF}'
}

# Invoke the helper exactly as the C++ does (sandbox CWD, sandbox PATH).
# $1 = stdin source; remaining args = the helper's args.
# Sets RC (exit code), OUT (stdout), MS (elapsed milliseconds).
invoke_helper() {
    local stdin_src="$1"; shift
    local t0 t1
    t0=$(date +%s%3N)
    RC=0
    OUT=$(cd "$SBX" && PATH="$BIN:$ESSENTIAL" K17_PIDFILE="$K17_PIDFILE" "$HELPER" "$@" < "$stdin_src") || RC=$?
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

# case 1: non-blocking (konsole) + sleep 1 — the user's build case
echo '== case 1: non-blocking (konsole) + sleep 1 — the helper must wait =='
reset_bin; make_fake_konsole
K17_PIDFILE="$SBX/kc1"; rm -f "$K17_PIDFILE"
mkfifo "$SBX/fifo1"
# Simulate the user pressing enter after the command finishes: feed one
# newline down the pipe the window's shell `read`s from at ~1.5 s.
( sleep 1.5; printf '\n'; sleep 20 ) > "$SBX/fifo1" &
feeder=$!
invoke_helper "$SBX/fifo1" 'sleep 1; read -p "Press enter to exit"'
kill "$feeder" 2>/dev/null || true
wait "$feeder" 2>/dev/null || true
file="$(extract_file)"
check_eq   'case 1: helper rc = 0 (launch contract)' "$RC" 0
check_ge   "case 1: helper waited for the command (elapsed ${MS}ms >= 1000ms)" "$MS" 1000
check_gone 'case 1: temp file self-cleaned by the terminal shell' "$file"
check_gone 'case 1: pid marker cleaned' "$file.pid"
check_eq   'case 1: fake konsole recorded the window child (D-Bus path used)' \
    "$([ -s "$K17_PIDFILE" ] && echo yes || echo no)" yes

# case 2: konsole + sleep 30, window closed after 1 s
echo '== case 2: non-blocking (konsole) + sleep 30, window close — safety net =='
reset_bin; make_fake_konsole
K17_PIDFILE="$SBX/kc2"; rm -f "$K17_PIDFILE"
t0=$(date +%s%3N)
( cd "$SBX" && PATH="$BIN:$ESSENTIAL" K17_PIDFILE="$K17_PIDFILE" \
    "$HELPER" 'sleep 30; read -p "Press enter to exit"' > "$SBX/out2" < /dev/null ) &
hp=$!
sleep 1.2
child=$(cat "$K17_PIDFILE" 2>/dev/null || true)
pkill -TERM -P "$child" 2>/dev/null || true   # the window's sleep
kill -TERM "$child" 2>/dev/null || true       # the window's command shell
RC=0; wait "$hp" || RC=$?
t1=$(date +%s%3N); MS=$(( t1 - t0 ))
OUT=$(cat "$SBX/out2")
file="$(extract_file)"
check_eq   'case 2: helper rc = 0' "$RC" 0
check_lt   "case 2: helper terminated after the window closed (elapsed ${MS}ms < 5000ms)" "$MS" 5000
check_gone 'case 2: temp file removed by the helper safety net' "$file"
check_gone 'case 2: pid marker removed by the helper safety net' "$file.pid"

# case 3: blocking (xterm) + command rc 7
echo '== case 3: blocking (xterm) + command rc 7 — rc propagation, no grace hang =='
reset_bin; make_fake_xterm
K17_PIDFILE=""
invoke_helper /dev/null 'exit 7; read -p "Press enter to exit"'
file="$(extract_file)"
check_eq   'case 3: helper rc = 7 (command rc propagated)' "$RC" 7
check_lt   "case 3: no grace hang (elapsed ${MS}ms < 1000ms)" "$MS" 1000
check_gone 'case 3: temp file removed (safety net — self-cleanup never ran after exit 7)' "$file"
check_gone 'case 3: pid marker removed (safety net)' "$file.pid"

# case 4: escalated (pkexec rootshell) + non-blocking (konsole)
echo '== case 4: escalated (pkexec rootshell) + non-blocking (konsole) — fire-and-forget =='
reset_bin; make_fake_konsole; make_fake_pkexec; make_rootshell
K17_PIDFILE="$SBX/kc4"; rm -f "$K17_PIDFILE"
invoke_helper /dev/null -s "pkexec $SBX/rootshell.sh" 'sleep 1; read -p "Press enter to exit"'
file="$(extract_file)"
check_eq   'case 4: helper rc = 0 (fire-and-forget launch contract)' "$RC" 0
check_lt   "case 4: helper returned immediately (elapsed ${MS}ms < 1000ms)" "$MS" 1000
for i in $(seq 1 20); do
    [ ! -e "$file" ] && break
    sleep 0.1
done
check_gone 'case 4: file self-removed by the "root" shell within 2 s' "$file"

# case 5: non-blocking (konsole) + failing command
echo '== case 5: non-blocking (konsole) + failing command (exit 5) — launch contract rc 0 =='
reset_bin; make_fake_konsole
K17_PIDFILE="$SBX/kc5"; rm -f "$K17_PIDFILE"
invoke_helper /dev/null 'exit 5; read -p "Press enter to exit"'
file="$(extract_file)"
check_eq   'case 5: helper rc = 0 (launch contract — the command rc is the terminal business)' "$RC" 0
check_lt   "case 5: helper exited promptly after the shell died (elapsed ${MS}ms < 3000ms)" "$MS" 3000
check_gone 'case 5: temp file removed by the helper safety net (self-cleanup never ran after exit 5)' "$file"
check_gone 'case 5: pid marker removed by the helper safety net' "$file.pid"

# case 6: konsole + the run_cmd_async CWD prefix — "cd '<dir>' && cmd"
# must run in the cd'd directory (the D-Bus window's profile CWD is
# irrelevant), and the marker must land there, not in the helper CWD.
echo "== case 6: non-blocking (konsole) + cd prefix — marker lands in the cd'd dir =="
reset_bin; make_fake_konsole
K17_PIDFILE="$SBX/kc6"; rm -f "$K17_PIDFILE"
mkdir -p "$SBX/sub"
# The sleep 1 models a command of finite duration (the app's build command
# runs for minutes; its trailing `read` blocks on interactive stdin, so the
# PID marker always outlives the helper's 0.2 s grace tick — an instant
# EOF-stdin command would race that tick and is not a real-app shape).
CMD6="cd '$SBX/sub' && touch made-it; sleep 1; read -p 'Press enter to exit'"
invoke_helper /dev/null "$CMD6"
file="$(extract_file)"
check_eq   'case 6: helper rc = 0 (launch contract)' "$RC" 0
check_ge   "case 6: helper waited for the command (elapsed ${MS}ms >= 1000ms)" "$MS" 1000
check_gone 'case 6: temp file self-cleaned' "$file"
check_gone 'case 6: pid marker cleaned' "$file.pid"
check_eq   "case 6: marker landed in the cd'd subdir" "$([ -f "$SBX/sub/made-it" ] && echo yes || echo no)" yes
check_eq   'case 6: no marker in the helper launch CWD' "$([ -f "$SBX/made-it" ] && echo yes || echo no)" no

# --- summary -------------------------------------------------------------------
echo
echo "k17: ${PASS} passed, ${FAIL} failed"
if [ "$FAIL" -ne 0 ]; then
    exit 1
fi
