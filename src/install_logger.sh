#!/usr/bin/env bash
#
# install_logger.sh — run one kernel-manager install step, logging its full
# output to the install log file while streaming it to the terminal in
# real time (the window's output is transient; the log is the post-mortem
# record).
#
# Usage: install_logger.sh <log_file> <step_label> <command>
#
#   <log_file>    the install log, created by kernel-manager before the
#                 first step (its directory is created here defensively)
#   <step_label>  e.g. "Step 1/3: pacman -U" or "Post-Install Verification"
#   <command>     the shell command to run, as ONE string: it may carry
#                 shell syntax (`||` fallbacks, `2>/dev/null` guards,
#                 quoted paths) and is therefore executed via `bash -c`,
#                 never word-split into separate arguments
#
# The step is appended to the log as:
#
#   --- <step_label> ---
#   Command: <command>
#   Started: <YYYY-MM-DD HH:MM:SS>
#   <blank>
#   Exit code: <the command's real exit code>
#   Output:
#   <full stdout+stderr>
#   <blank>
#
# The output is tee'd to the terminal (live) and to a temp file; the temp
# file is appended to the log only after the command finishes, so the log
# keeps the header → output → footer order no matter how long the command
# ran. The script exits with the command's own exit code (the caller then
# runs its "Press enter to exit" prompt).

set -uo pipefail

if [[ $# -lt 3 ]]; then
    echo "usage: install_logger.sh <log_file> <step_label> <command>" >&2
    exit 2
fi

LOG_FILE="$1"
STEP_LABEL="$2"
COMMAND="$3"

# The step header. (The log file itself is created by kernel-manager; the
# mkdir is belt-and-suspenders for a cache dir wiped mid-install.)
mkdir -p "$(dirname "$LOG_FILE")" 2>/dev/null || true
{
    echo ""
    echo "--- $STEP_LABEL ---"
    echo "Command: $COMMAND"
    echo "Started: $(date '+%Y-%m-%d %H:%M:%S')"
    echo ""
} >> "$LOG_FILE"

# Run the command through a shell (see the usage note on <command>).
# stdout+stderr stream to the terminal (live) and to the temp file. No
# `set -e`: a failing command must still be logged, not abort the script.
TMP_OUTPUT="$(mktemp)" || TMP_OUTPUT="/dev/null"
bash -c "$COMMAND" 2>&1 | tee "$TMP_OUTPUT"
# PIPESTATUS[0] = the command's rc (not the pipeline's: with pipefail, $?.
# The temp file is a writable sink — tee's own rc never matters here).
RC=${PIPESTATUS[0]}

# The step footer: the real exit code + the captured output.
{
    echo "Exit code: $RC"
    echo "Output:"
    cat "$TMP_OUTPUT"
    echo ""
} >> "$LOG_FILE"

rm -f "$TMP_OUTPUT"
exit "$RC"
