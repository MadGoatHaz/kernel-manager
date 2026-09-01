#!/usr/bin/env bash
# build_helper.sh — makepkg wrapper with automatic GPG key import.
# Usage: build_helper.sh <makepkg args...>
# Runs makepkg; if it fails with "unknown public key", imports the key and retries.
set -uo pipefail

run_makepkg_with_gpg_fix() {
    local output
    if output=$(makepkg "$@" 2>&1); then
        echo "$output"
        return 0
    fi
    echo "$output"

    # Extract unknown key ID(s) from the error output
    local key_ids
    key_ids=$(echo "$output" | grep -oP 'unknown public key \K[0-9A-F]+' | sort -u)
    
    if [[ -n "$key_ids" ]]; then
        echo ""
        for key_id in $key_ids; do
            echo "Importing GPG key $key_id…"
            gpg --recv-keys "$key_id" 2>&1 || true
            # Mark as ultimately trusted
            echo "$key_id:5" | gpg --import-ownertrust 2>/dev/null || true
        done
        echo "Retrying makepkg…"
        makepkg "$@"
        return $?
    fi

    # No GPG key issue — propagate the original failure
    return 1
}

run_makepkg_with_gpg_fix "$@"
