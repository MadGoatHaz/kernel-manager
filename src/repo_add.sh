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
SERVERS=()
case "$REPO" in
    cachyos)
        SERVERS=(
            "https://cdn77.cachyos.org/repo/\$arch/\$repo"
            "https://us.cachyos.org/repo/\$arch/\$repo"
        )
        ;;
    chaotic-aur)
        SERVERS=(
            "https://geo-mirror.chaotic.cx/\$repo/\$arch"
            "https://us-mi-mirror.chaotic.cx/\$repo/\$arch"
        )
        ;;
    liquorix)
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

# --- pacman -Sy with automatic GPG key import ------------------------------
# The refresh can fail when the repo's signing key is not yet in the keyring
# (the live case: `error: cachyos: key "882DCFE4…" is unknown`). In that one
# situation the key is fetched + trusted and the refresh retried; any other
# failure (network, mirror, DB error, a second failure after the retry)
# surfaces the pacman output and the script exits non-zero. The pacman call
# sits in a conditional context so `set -e` does not abort mid-function.
# --dry-run never reaches here (it exits before the refresh); both real
# paths (fresh append + already-enabled re-run) do.
run_pacman_sy() {
    local output
    if output=$(pacman -Sy 2>&1); then
        echo "$output"
        return 0
    fi
    echo "$output"

    # Extract the unknown key ID (pattern: key "<40-hex>" …). `|| true`
    # guards set -e when the pattern is absent — a non-GPG failure must
    # reach the final return 1 with its text already printed, not abort
    # at this assignment.
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
