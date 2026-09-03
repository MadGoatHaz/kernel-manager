#!/usr/bin/env bash
#
# postinstall_tail.sh — kernel-manager consolidated post-install tail (phase b).
#
# ONE root script (a single pkexec, never multiple escalated windows) that runs
# after the package install (phase a) succeeded. Stages:
#   (1) KVER derivation — the NEW kernel's version, from the package's .PKGINFO,
#       never uname (R1);
#   (2) driver sync — the FULL nvidia family + dkms + zfs, per-package guarded
#       (D-B); drivers MUST precede the initramfs;
#   (3) initramfs — targets <KVER> (chunk B: reinstall-kernels →
#       dracut --force --kver → mkinitcpio -k, each command -v-probed);
#   (4) verify + verdict — chunk C (extramodules/ + updates/dkms/ module probe,
#       per-<KVER> initramfs non-empty, BLS/grub check).
#
# Verdict rc (captured by the terminal-helper sentinel wrapper, which runs
# `echo $? > rc` after this script — the exit code IS the verdict):
#   0 = BOOT_SAFE
#   1 = INSTALLED_NOT_BOOT_SAFE
# (INSTALLATION_FAILED is the separate phase-a pacman step, never this script.)
#
# Every stage is guarded: NO set -e — a failed sub-action sets the verdict and
# the script runs on to write it (a failed stage must not abort before the
# verdict is emitted).
#
# Usage: postinstall_tail.sh <PKGNAME> <KVER|empty> <BOOTLOADER> [VERDICT_FILE]
#   PKGNAME      installed package name (the phase-a target, e.g. "linux")
#   KVER         the new kernel version; empty ⇒ derived from the local DB
#   BOOTLOADER   systemd-boot | grub | uki | unknown (chunk C consumes it)
#   VERDICT_FILE optional: the verdict is also written to this file (the
#                 sentinel's rc path) in addition to being the exit code

set -uo pipefail

PKGNAME="$1"
KVER="${2:-}"
BL="$3"
VERDICT_FILE="${4:-}"

log() { echo "[postinstall-tail] $*"; }

# The sentinel wrapper (terminal-helper) runs this script and then `echo $? > rc`,
# so the exit code IS the verdict the app reads. Mirror it to VERDICT_FILE too
# when one was given (the spec's rc path).
finish() {
    local rc="$1"
    if [ -n "$VERDICT_FILE" ]; then
        echo "$rc" > "$VERDICT_FILE" || true
    fi
    exit "$rc"
}

# --- (1) KVER derivation (R1: from the package's .PKGINFO, never uname) -----
# A directory install passes the .PKGINFO pkgver as $2; a repo install derives
# it here from the local DB of the package just installed by phase a.
if [ -z "$KVER" ]; then
    KVER=$(awk -F' = ' '/^pkgver/{print $2}' "/var/lib/pacman/local/$PKGNAME"/*/.PKGINFO 2>/dev/null | head -1)
fi
if [ -z "$KVER" ]; then
    log "ERROR: cannot derive KVER for '$PKGNAME' (no pkgver in the local DB) — a phase-a install failure surfacing here"
    finish 1
fi
log "KVER: $KVER (pkg: $PKGNAME, bootloader: ${BL:-unknown})"

# --- (2) driver stage (D-B): the full nvidia family + dkms, per-package guarded
# For the DEFAULT (official) kernel the nvidia driver is the BINARY package
# (modules land in extramodules/); for a custom kernel it is the DKMS variant
# (modules land in updates/dkms/). Re-pulling the installed binary package
# re-fetches the .ko for <KVER>; when NVIDIA ships no .ko for a custom version
# the reinstall fails ⇒ VERDICT=1 and chunk C's verify stage confirms the gap
# (binary-on-custom is INSTALLED_NOT_BOOT_SAFE, not a broken install).
# Drivers sync BEFORE the initramfs: a module that lands after initramfs
# generation is missing from the new kernel's initramfs ⇒ the system won't boot.
# Each sub-action is guarded by `pacman -Q`; a failure logs + sets the verdict
# and never aborts (no set -e).
VERDICT=0

for p in nvidia nvidia-open nvidia-lts nvidia-open-lts; do
    if pacman -Q "$p" 2>/dev/null; then
        log "driver: re-installing binary package '$p' (re-pull the .ko for $KVER)"
        if ! pacman -U --noconfirm "$p"; then
            log "WARN: 'pacman -U --noconfirm $p' failed (no .ko for $KVER? — the verify stage will confirm the gap)"
            VERDICT=1
        fi
    fi
done

for p in nvidia-dkms nvidia-open-dkms; do
    if pacman -Q "$p" 2>/dev/null; then
        log "driver: dkms autoinstall for '$p' targeting $KVER"
        if ! dkms autoinstall -k "$KVER"; then
            log "WARN: 'dkms autoinstall -k $KVER' failed for '$p'"
            VERDICT=1
        fi
    fi
done

if pacman -Q zfs 2>/dev/null; then
    log "driver: dkms autoinstall for 'zfs' targeting $KVER"
    if ! dkms autoinstall -k "$KVER"; then
        log "WARN: 'dkms autoinstall -k $KVER' failed for 'zfs'"
        VERDICT=1
    fi
fi

# Trailing marker line the app can surface/grep:
log "DRIVER_STAGE_DONE verdict=$VERDICT kver=$KVER"

# --- (3) initramfs stage — chunk B inserts it HERE, after the drivers --------
# (reinstall-kernels [EOS] → dracut --force --kver $KVER → mkinitcpio -k $KVER,
#  each command -v-probed; only the first available runs; a missing tool ⇒ 1)

# --- (4) verify stage + 3-state verdict output — chunk C inserts it HERE -----
# (extramodules/ + updates/dkms/ module probe for $KVER, per-$KVER initramfs
#  non-empty, BLS entry / grub-mkconfig for $BL; then the final VERDICT line)

finish "$VERDICT"
