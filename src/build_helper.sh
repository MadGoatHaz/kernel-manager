#!/usr/bin/env bash
# build_helper.sh — makepkg wrapper with real-time output streaming and
# automatic GPG key import.
# Usage: build_helper.sh <makepkg args...>
#
# All makepkg output is streamed to the terminal AND a persistent log file
# via tee, so a long kernel build (20-90+ min) never looks hung: every line
# appears as it is produced. The log file lives at
#   ${XDG_CACHE_HOME:-~/.cache}/kernel-manager/build-<timestamp>-<pid>.log
# and survives the build for post-mortem analysis. The $$ PID suffix keeps
# concurrent builds from sharing (and truncating) one log file.
#
# makepkg is always run with --noconfirm: without it, interactive pacman
# prompts are the only thing that reaches the tty and the build stalls on
# an invisible "proceed?" question.
#
# If makepkg fails on a missing GPG key, the key is imported (3-needle
# extraction from the log file, first non-empty wins) and makepkg is
# retried once:
#   1. "unknown public key <ID>" — libalpm's pacman-style line
#   2. "… is not in your keyring" — gpg names the key ID on the same line
#   3. "No public key" (Can't check signature) — gpg; the key ID rides on
#      the paired "Signature made … using <algo> key ID <hex>" line
# If needle 3 matches but no key ID is extractable, a manual-import hint is
# printed and one retry is still made; any other failure propagates as-is.
set -uo pipefail

# Log file in the user's cache directory.
LOG_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/kernel-manager"
mkdir -p "$LOG_DIR"
LOG_FILE="$LOG_DIR/build-$(date +%Y%m%d-%H%M%S)-$$.log"

# Run makepkg with streaming output + GPG retry.
run_makepkg_with_gpg_fix() {
    local attempt=0
    local max_attempts=2

    while [[ $attempt -lt $max_attempts ]]; do
        attempt=$((attempt + 1))

        # Stream output to the terminal AND the log file. tee keeps the user
        # seeing progress in real-time; the log keeps everything for later.
        # --noconfirm prevents invisible pacman prompts. PIPESTATUS[0] is
        # makepkg's exit code (tee's own is not what we want).
        makepkg "$@" --noconfirm 2>&1 | tee -a "$LOG_FILE"
        local rc=${PIPESTATUS[0]}

        if [[ $rc -eq 0 ]]; then
            return 0
        fi

        # Extract the missing key ID(s) from the log file — first non-empty
        # wins:
        local key_ids=""
        # Needle 1: libalpm "unknown public key <ID>"
        key_ids=$(grep -oP 'unknown public key \K[0-9A-F]+' "$LOG_FILE" 2>/dev/null | sort -u)
        # Needle 2: gpg "key <ID>: … is not in your keyring" — 8-40 hex
        # (short keyid … full fingerprint); --recv-keys accepts all forms.
        if [[ -z "$key_ids" ]]; then
            key_ids=$(grep -F 'is not in your keyring' "$LOG_FILE" 2>/dev/null | grep -oiP '[0-9A-F]{8,40}' | sort -u)
        fi

        # Needle 3: "No public key" — the key ID is on the paired
        # "Signature made … using <algo> key ID <hex>" line.
        local no_public_key=0
        if grep -qF 'No public key' "$LOG_FILE" 2>/dev/null; then
            no_public_key=1
            if [[ -z "$key_ids" ]]; then
                key_ids=$(grep -oiP '(RSA|ECDSA|ED25519)[^0-9A-F]*key ID \K[0-9A-F]+' "$LOG_FILE" 2>/dev/null | sort -u)
            fi
        fi

        if [[ -n "$key_ids" ]]; then
            echo ""
            echo "=== Importing GPG keys: $key_ids ===" | tee -a "$LOG_FILE"
            for key_id in $key_ids; do
                gpg --recv-keys "$key_id" 2>&1 | tee -a "$LOG_FILE" || true
                # Mark as ultimately trusted
                echo "$key_id:5" | gpg --import-ownertrust 2>/dev/null || true
            done
            echo "=== Retrying makepkg… ===" | tee -a "$LOG_FILE"
            # Mark the retry in the log (the failure output above it stays)
            echo "" >> "$LOG_FILE"
            echo "=== RETRY ATTEMPT $attempt ===" >> "$LOG_FILE"
            continue
        fi

        # "No public key" but no ID extractable: no blind import — print a
        # manual-import hint, then retry once (the key may have been
        # imported in the meantime).
        if [[ $no_public_key -eq 1 ]]; then
            echo ""
            echo "Cannot extract the missing GPG key ID — import it manually (gpg --recv-keys <id>), then re-run the build." | tee -a "$LOG_FILE"
            echo "Retrying makepkg… (the key may need to be imported first)" | tee -a "$LOG_FILE"
            echo "" >> "$LOG_FILE"
            echo "=== RETRY ATTEMPT $attempt (manual key import hint) ===" >> "$LOG_FILE"
            continue
        fi

        # Non-GPG failure — no retry; propagate makepkg's exit code.
        echo ""
        echo "=== makepkg failed (exit code $rc). Log: $LOG_FILE ===" | tee -a "$LOG_FILE"
        return $rc
    done

    # Retry exhausted (the imported keys did not fix the failure)
    echo ""
    echo "=== makepkg failed after $max_attempts attempts. Log: $LOG_FILE ===" | tee -a "$LOG_FILE"
    return 1
}

echo "=== Build started: $(date) ===" | tee "$LOG_FILE"
echo "=== Log file: $LOG_FILE ==="
run_makepkg_with_gpg_fix "$@"
exit $?
