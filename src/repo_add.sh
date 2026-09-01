#!/usr/bin/env bash
# repo_add.sh — enable a pacman repo by appending its [section] to /etc/pacman.conf.
#
# Runs as root via the pkexec run-root-terminal (auth_admin) rootshell layer
# (org.archlinux.kernel-manager polkit policy → /usr/lib/kernel-manager/rootshell.sh),
# launched by the kernel-manager app's "Add repo '<name>'" context action
# (utils::add_repo_to_pacman_conf → "$KM_HELPER_DIR/repo_add.sh '<repo>'").
#
# Usage: repo_add.sh <repo> [--conf <path>] [--dry-run]
#   <repo>       one of the static allowlist below (cachyos | chaotic-aur | liquorix)
#   --conf <p>   target config file (test hook; default /etc/pacman.conf)
#   --dry-run    print the would-be section + the would-run command; no writes
#
# The allowlist is the last wall: the C++ layer guards the input (empty / "aur" /
# charset [a-z0-9-] / curated-table membership) and the quote in the launched
# command is inert because of it — and even so, only the three names below can
# ever reach this file; nothing else from the caller is written to disk.
#
# Server URLs verified 2026-08-31 against the official sources (fetch evidence in
# the C6 entry of Work/DEV_LOG.md — the plan's original URLs are all dead):
#   cachyos      official cachyos-mirrorlist pkg (v27, mirror.cachyos.org) — the
#                repo.cachyos.org / openrepo.cachyos.org hosts no longer resolve
#   chaotic-aur  official chaotic-mirrorlist pkg (20240724) — aur.chaotic.cx no
#                longer serves the repo (SPA soft-404); pattern is <mirror>.chaotic.cx
#   liquorix     official install-liquorix.sh (liquorix.net) — the older
#                repos/packman/arch layout 404s; live repo is liquorix.net/archlinux
set -euo pipefail

REPO=""
CONF="/etc/pacman.conf"
DRY_RUN=0

usage() {
    echo "Usage: $0 <repo> [--conf <path>] [--dry-run]" >&2
}

# --- arg parse -----------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --conf)
            if [[ $# -lt 2 ]]; then
                echo "Error: --conf requires a path" >&2
                usage
                exit 1
            fi
            CONF="$2"
            shift 2
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        -*)
            echo "Error: unknown option '$1'" >&2
            usage
            exit 1
            ;;
        *)
            if [[ -n "$REPO" ]]; then
                echo "Error: multiple repos given ('$REPO' and '$1')" >&2
                usage
                exit 1
            fi
            REPO="$1"
            shift
            ;;
    esac
done

if [[ -z "$REPO" ]]; then
    echo "Error: no repo given" >&2
    usage
    exit 1
fi

# --- static allowlist (the last wall) -------------------------------------
# $arch/$repo are PACMAN variables: they must stay literal in the conf, so the
# bash strings below escape them (\$) — they are expanded only by pacman at
# sync time, never by this script.
#
# REPO_KEY = the full 40-hex fingerprint of the repo's signing key, imported +
# trusted PROACTIVELY before the first `pacman -Sy` (run_pacman_sy) so a
# first-time repo enable does not fail on the unknown-key / unknown-trust GPG
# errors (see the libalpm message strings cited in run_pacman_sy). Verified
# 2026-08-31 against each repo's live signatures (evidence in the C6 entry of
# Work/DEV_LOG.md, extended for the key IDs):
#   cachyos     key "882DCFE4…8DB35A47" — the exact ID pacman names in its
#               `key "…" is unknown` error; the live cachyos.db.tar.zst.sig
#               is signed with keyid F3B607488DB35A47 (its 16-hex suffix)
#   chaotic-aur primary "67BF8CA6…37365605" (TNE <tne@garudalinux.org>,
#               the garuda/chaotic keyring key); packages are signed by its
#               [S] subkey 349BC7808577C592 — trusting the PRIMARY covers
#               the subkey (verified: after a master-key lsign of the
#               primary, the subkey-signed package verifies [full])
#   liquorix    primary "C5ADB4F3…33F8024D" (Steven Barrett
#               <steven@liquorix.net>, [SC]); matches the 9AE4078033F8024D
#               key that the official install-liquorix.sh imports
SERVERS=()
REPO_KEY=""
case "$REPO" in
    cachyos)
        REPO_KEY="882DCFE48E2051D48E2562ABF3B607488DB35A47"
        SERVERS=(
            "https://cdn77.cachyos.org/repo/\$arch/\$repo"
            "https://us.cachyos.org/repo/\$arch/\$repo"
        )
        ;;
    chaotic-aur)
        REPO_KEY="67BF8CA6DA181643C9723B4ED6C9442437365605"
        SERVERS=(
            "https://geo-mirror.chaotic.cx/\$repo/\$arch"
            "https://us-mi-mirror.chaotic.cx/\$repo/\$arch"
        )
        ;;
    liquorix)
        REPO_KEY="C5ADB4F3FEBBCE27A3E54D7D9AE4078033F8024D"
        SERVERS=(
            "https://liquorix.net/archlinux/\$repo/\$arch"
        )
        ;;
    *)
        echo "Error: unknown repo '$REPO'" >&2
        exit 1
        ;;
esac

# The would-be [section] (pacman variables $arch/$repo stay literal in the conf).
# printf -v (not $(printf …)): command substitution strips trailing newlines,
# which would glue the [header] to the first Server line — an invalid section.
printf -v SECTION '\n[%s]\n' "$REPO"
for s in "${SERVERS[@]}"; do
    printf -v line 'Server = %s\n' "$s"
    SECTION+=$line
done

# --- pacman -Sy: proactive key trust + automatic GPG-error repair ----------
# The refresh can fail on GPG in TWO ways, both repaired here:
#   case 1 — the signing key is absent from the keyring. libalpm names the
#      key: `error: <repo>: key "<40-hex>" is unknown`. The key is fetched +
#      trusted and the refresh retried.
#   case 2 — the key is present but untrusted. libalpm names the SIGNER,
#      not the key: `error: <repo>: signature from "<uid>" is unknown trust`
#      (marginal trust likewise). Case 1's key-ID parse cannot see this, so
#      the key is looked up by that uid in the pacman keyring, trusted, and
#      the refresh retried.
# Before the first attempt, REPO_KEY (set in the allowlist) is imported +
# trusted PROACTIVELY, so a known repo's first enable rarely reaches either
# reactive case. Any other failure (network, mirror, DB error, an
# unrepairable GPG error, a second failure after the retry) surfaces the
# pacman output and the script exits non-zero. Every pacman call sits in a
# conditional context (and every pacman-key call is `|| true`- or
# `&&`-guarded) so `set -e` does not abort mid-function. --dry-run never
# reaches here (it exits before the refresh); both real paths (fresh append
# + already-enabled re-run) do.
run_pacman_sy() {
    # Proactive: import + trust this repo's GPG key before the first
    # refresh. Best-effort by design — the key may already be in the
    # keyring (recv is a no-op), the keyserver may be unreachable (a
    # pre-existing trusted key still lets the refresh proceed), or the key
    # may already be trusted (lsign is a no-op); none of that may abort
    # the refresh. Stderr is hidden: if it matters, the reactive cases
    # below re-report it with the real error.
    if [[ -n "$REPO_KEY" ]]; then
        echo "Importing GPG key $REPO_KEY…"
        pacman-key --recv-keys "$REPO_KEY" 2>/dev/null || true
        pacman-key --lsign-key "$REPO_KEY" 2>/dev/null || true
    fi

    local output
    if output=$(pacman -Sy 2>&1); then
        echo "$output"
        return 0
    fi
    echo "$output"

    # Case 1: key absent — extract the unknown key ID (pattern:
    # key "<40-hex>" …). `|| true` guards set -e when the pattern is
    # absent — a non-GPG failure must reach the final return 1 with its
    # text already printed, not abort at this assignment.
    local key_id
    key_id=$(echo "$output" | grep -oP 'key "\K[0-9A-F]{40}' | head -1 || true)
    if [[ -n "$key_id" ]]; then
        echo ""
        echo "Importing GPG key $key_id…"
        if pacman-key --recv-keys "$key_id" && pacman-key --lsign-key "$key_id"; then
            echo "Retrying pacman -Sy…"
            if pacman -Sy; then
                return 0
            fi
        fi
    fi

    # Case 2: key present but untrusted — the error names the signer's uid
    # ("signature from \"<uid>\" is unknown trust"), never the key ID, so
    # case 1's parse finds nothing. Look the key up by uid in the pacman
    # keyring (gpg matches the uid string; the first 16-hex ID line of the
    # listed block is the primary key, and trusting it covers any subkey
    # that signed the package) and lsign it.
    if echo "$output" | grep -qE 'is (unknown|marginal) trust'; then
        local signer trust_id
        signer=$(echo "$output" | grep -oP 'signature from "\K[^"]+' | head -1 || true)
        if [[ -n "$signer" ]]; then
            trust_id=$(pacman-key --list-keys "$signer" 2>/dev/null | grep -oE '[0-9A-F]{16}' | head -1 || true)
            if [[ -n "$trust_id" ]]; then
                echo ""
                echo "Signing untrusted key $trust_id ($signer)…"
                if pacman-key --lsign-key "$trust_id"; then
                    echo "Retrying pacman -Sy…"
                    if pacman -Sy; then
                        return 0
                    fi
                fi
            fi
        fi
    fi
    return 1
}

# --- idempotency: an active [section] already present ⇒ no append, no backup ---
if [[ -f "$CONF" ]] && grep -qE "^[[:space:]]*\[$REPO\]" "$CONF"; then
    echo "Repo '$REPO' already enabled in $CONF — skipping append (no backup)."
    if [[ "$DRY_RUN" -eq 1 ]]; then
        echo "[dry-run] would run: pacman -Sy"
        exit 0
    fi
    echo "Running pacman -Sy…"
    run_pacman_sy
    exit 0
fi

# --- dry-run: print the full plan, no writes ------------------------------
if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[dry-run] would back up: $CONF → $CONF.bak-<timestamp>"
    echo "[dry-run] would append to $CONF:"
    printf '%s' "$SECTION"
    echo "[dry-run] would run: pacman -Sy"
    exit 0
fi

# --- real path: backup, then append, then refresh the DBs ------------------
if [[ ! -f "$CONF" ]]; then
    echo "Error: config file '$CONF' not found" >&2
    exit 1
fi

BACKUP="$CONF.bak-$(date +%Y%m%d%H%M%S)"
cp -a "$CONF" "$BACKUP"
echo "Backed up $CONF → $BACKUP"

printf '%s' "$SECTION" >> "$CONF"
echo "Appended [$REPO] to $CONF"

echo "Running pacman -Sy…"
run_pacman_sy
