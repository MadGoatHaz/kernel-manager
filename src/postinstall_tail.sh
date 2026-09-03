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
#   (3) initramfs — targets <KVER> (reinstall-kernels →
#       dracut --force --kver → mkinitcpio -k, each command -v-probed; the
#       ESP machine-id layout is deployed explicitly when it exists);
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

# Deploy the freshly generated initramfs to the systemd-boot ESP machine-id
# layout (/efi/<machine-id>/<KVER>/initrd) when that layout exists
# (EndeavourOS / systemd-boot installs). `dracut` and `mkinitcpio` write
# their output to /boot only, but the BLS entry on such systems references
# the ESP copy — without this deploy the new kernel would boot with the
# STALE initrd (missing the just-rebuilt driver modules) ⇒ not boot-safe.
# reinstall-kernels needs no such step: kernel-install deploys the ESP copy
# itself. No-op (return 0) when the layout is absent (grub/UKI systems read
# the initramfs from /boot). A failed deploy sets the verdict, never aborts
# (no set -e).
deploy_esp_initrd() {
    local kver="$1" mid src esp_layout c
    mid=$(cat /etc/machine-id 2>/dev/null)
    if [ -z "$mid" ] || [ ! -d "/efi/$mid/$kver" ]; then
        return 0
    fi
    esp_layout="/efi/$mid/$kver/initrd"
    # Fresh image candidates: dracut names it initramfs-<kver>.img (in /boot,
    # or / when the boot files live on the root fs); mkinitcpio may append
    # the arch (initramfs-<kver>-<arch>.img) — take the first non-empty one.
    src=""
    for c in "/boot/initramfs-$kver.img" "/initramfs-$kver.img" \
             /boot/initramfs-"$kver".* /initramfs-"$kver".*; do
        if [ -s "$c" ]; then
            src="$c"
            break
        fi
    done
    if [ -z "$src" ]; then
        log "WARN: esp-deploy: no fresh non-empty initramfs for $kver in /boot — the ESP keeps the stale initrd"
        VERDICT=1
        return 0
    fi
    if cp -f "$src" "$esp_layout"; then
        log "esp-deploy: $src -> $esp_layout (the BLS entry now reads the fresh initrd)"
    else
        log "WARN: esp-deploy: failed to copy $src -> $esp_layout"
        VERDICT=1
    fi
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

# --- (3) initramfs stage (D-C): targets the NEW kernel version $KVER --------
# Reuses the $KVER derived above (from the package, never uname), and runs
# AFTER the driver stage: a module that lands after the initramfs generation
# is missing from the new kernel's initramfs ⇒ the system won't boot.
# Tool preference — only the first available runs (each command -v-probed, so
# the stage is safe on any pacman-family distro even if the family detection
# is wrong):
#   reinstall-kernels   the EndeavourOS blessed repair (kernel-install add per
#                       installed kernel; deploys the initrd to the ESP
#                       machine-id layout and refreshes the BLS entry itself)
#   dracut --force --kver  Arch/EOS: regenerate ONLY $KVER's initramfs (NOT
#                       bare -f, which targets the RUNNING kernel — C2); the
#                       ESP machine-id layout is then deployed explicitly
#   mkinitcpio -k       plain Arch/Manjaro (mkinitcpio-based distros; NOT -P,
#                       which regenerates all installed kernels)
# No --add/force_drivers (D-C reject): dracut hard-fails when a forced module
# is missing, so a missing driver is reported by the verdict instead.
# A failed sub-action logs + sets the verdict and never aborts (no set -e).
if command -v reinstall-kernels >/dev/null 2>&1; then
    log "initramfs: reinstall-kernels (EOS blessed; kernel-install handles the ESP deploy)"
    if ! reinstall-kernels; then
        log "WARN: 'reinstall-kernels' failed — the new kernel may not be boot-safe"
        VERDICT=1
    fi
elif command -v dracut >/dev/null 2>&1; then
    log "initramfs: dracut --force --kver $KVER"
    if ! dracut --force --kver "$KVER"; then
        log "WARN: 'dracut --force --kver $KVER' failed — the new kernel may not be boot-safe"
        VERDICT=1
    else
        deploy_esp_initrd "$KVER"
    fi
elif command -v mkinitcpio >/dev/null 2>&1; then
    log "initramfs: mkinitcpio -k $KVER"
    if ! mkinitcpio -k "$KVER"; then
        log "WARN: 'mkinitcpio -k $KVER' failed — the new kernel may not be boot-safe"
        VERDICT=1
    else
        deploy_esp_initrd "$KVER"
    fi
else
    log "ERROR: no initramfs tool available (reinstall-kernels/dracut/mkinitcpio all missing) — the new kernel has no initramfs ⇒ not boot-safe"
    VERDICT=1
fi

# Trailing marker line the app can surface/grep:
log "INITRAMFS_STAGE_DONE verdict=$VERDICT kver=$KVER"

# --- (4) verify stage + 3-state verdict output — chunk C inserts it HERE -----
# (extramodules/ + updates/dkms/ module probe for $KVER, per-$KVER initramfs
#  non-empty, BLS entry / grub-mkconfig for $BL; then the final VERDICT line)

finish "$VERDICT"
