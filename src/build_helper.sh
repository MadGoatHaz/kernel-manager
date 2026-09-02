#!/usr/bin/env bash
# build_helper.sh — makepkg wrapper with automatic GPG key import.
# Usage: build_helper.sh <makepkg args...>
# Runs makepkg; if it fails on a missing GPG key, imports the key and
# retries once. Three failure needles are recognized (first non-empty
# extraction wins):
#   1. "unknown public key <ID>" — libalpm's pacman-style line
#   2. "… is not in your keyring" — gpg names the key ID on the same line
#   3. "No public key" (Can't check signature) — gpg; the key ID rides on
#      the paired "Signature made … using <algo> key ID <hex>" line
# If needle 3 matches but no key ID is extractable, a manual-import hint is
# printed and one retry is still made; any other failure propagates as-is.
set -uo pipefail

run_makepkg_with_gpg_fix() {
    local output
    if output=$(makepkg "$@" 2>&1); then
        echo "$output"
        return 0
    fi
    echo "$output"

    # Extract the missing key ID(s) from the failure output — first non-empty wins:
    local key_ids
    key_ids=$(echo "$output" | grep -oP 'unknown public key \K[0-9A-F]+' | sort -u)
    if [[ -z "$key_ids" ]]; then
        # Needle 2: gpg "key <ID>: … is not in your keyring" — 8-40 hex
        # (short keyid … full fingerprint); --recv-keys accepts all forms.
        key_ids=$(echo "$output" | grep -F 'is not in your keyring' | grep -oiP '[0-9A-F]{8,40}' | sort -u)
    fi
    if [[ -z "$key_ids" ]]; then
        # Needle 3: "No public key" — the key ID is on the paired
        # "Signature made … using <algo> key ID <hex>" line.
        key_ids=$(echo "$output" | grep -oiP '(RSA|ECDSA|ED25519)[^0-9A-F]*key ID \K[0-9A-F]+' | sort -u)
    fi

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

    # "No public key" but no ID extractable: no blind retry — print a
    # manual-import hint, then retry once (the key may have been imported
    # in the meantime).
    if [[ "$output" == *'No public key'* ]]; then
        echo "Cannot extract the missing GPG key ID — import it manually (gpg --recv-keys <id>), then re-run the build."
        echo "Retrying makepkg…"
        makepkg "$@"
        return $?
    fi

    # No GPG key issue — propagate the original failure
    return 1
}

run_makepkg_with_gpg_fix "$@"
