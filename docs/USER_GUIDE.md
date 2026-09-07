# kernel-manager — User Guide

kernel-manager is a Qt6 desktop application for managing Linux kernels on Arch Linux. It installs, removes, and configures 21 curated kernel variants (official Arch, CachyOS, and community), builds custom kernels with `makepkg` from per-kernel options, shows the booted kernel's build telemetry at a glance, and guards GPU driver packaging before every install. It runs as your user and escalates to root only for pacman operations, via polkit.

## Installation

### From the AUR

`kernel-manager` is published on the [AUR](https://aur.archlinux.org/packages/kernel-manager):

```sh
yay -S kernel-manager
# or
paru -S kernel-manager
```

### System requirements

- An Arch Linux or Arch-based distribution — Arch, EndeavourOS, Manjaro, CachyOS, and Garuda are detected by name; any other distro with `ID_LIKE=arch` in `/etc/os-release` is treated as Arch.
- Qt6, `pacman`, `glib2`, and `polkit` — all package dependencies, installed automatically with the AUR build.
- A terminal emulator — privileged operations open one so you can watch and verify each step.
- Optional: `scx-manager` (sched-ext / BPF scheduler management, `optdepends`), an AUR helper such as `paru` (building AUR-sourced kernels), `git` (cloning build sources).

Building from source is documented in the [README](../README.md#installing).

## Main Window

The window (titled `Kernel Manager vX.Y.Z`, version also in the status bar) is arranged top to bottom:

1. **Active Kernel Information card** — a read-only 4×4 grid with 15 values about the kernel currently booted (details in [Active Kernel Telemetry](#active-kernel-telemetry)).
2. **Instruction line** — "Select kernels to install or remove, then click Execute."
3. **Pacman-lock banner** — an amber warning shown only while another pacman instance holds the database lock; it disappears within 2 seconds of the lock releasing (polled every 2 s). It is warning-only: the Execute button is never disabled by it.
4. **Kernel list** — one row per kernel, with columns:
   - *Choose* — checkbox for live rows; a lock glyph on "info-rows" (curated kernels not currently in an enabled repo, which cannot be selected);
   - *PkgName* — `repo/name` (e.g. `core/linux-zen`), with a hover tooltip describing the kernel;
   - *Version* — the repo version; `∧` prefix means an update is available, `∨` means the installed version is newer; `—` means the package is not in its (enabled) DB — run `pacman -Sy`; `— (repo not enabled)` means the repo is missing from `/etc/pacman.conf` — right-click the row to add it;
   - *Category* — derived from the name (e.g. `lto optimized`, `longterm`, `zen-kernel`, `mainline`, `handheld`);
   - *Installed* — `✓` if present in the local package DB, `—` otherwise.

   Rows sort on click of any column; **Space** or double-click toggles the current row's checkbox; single selection. A final pseudo-row, **"Install from directory…"** (folder icon), is the entry point for installing locally built packages.
5. **Build dir row** — `Build dir: <path>` (where clones and built packages live) with a **Browse…** button; the choice is remembered between runs.
6. **Action buttons** — `sched-ext scheduler config` (see below), `Configure`, `Refresh`, `Close`, and `Execute`.
7. **Status bar** — the app version, plus a persistent warning banner that appears when you decline a GPU driver fix (see [Driver gate](#driver-gate-gpu-conflict-detection)).

Privileged commands (install, remove, add-repo, driver migration, `pacman -U` of local packages) run in an **external terminal** opened via polkit; you watch the real output and the terminal blocks with "Press enter to exit". That terminal output is the source of truth for every operation.

## Features

### Install a Kernel

Three entry points:

- **Check + Execute** — tick the checkbox on one or more rows and click **Execute**. Repository kernels install with `pacman -S --needed <packages>` (elevated); AUR-sourced kernels are built first (`makepkg`, as your user). The list re-scans itself automatically after the transaction completes.
- **Right-click → "Install pre-compiled \<name\>"** — offered only when the kernel has a documented pre-compiled package *and* it is currently available (its repo is enabled, or the AUR tooling works). After a confirmation dialog, it installs the package plus its `-headers` companion in one transaction, then reports a two-state result: green **"Kernel installed successfully. You can reboot when ready."** or red **"Kernel installation failed (rc=N). Check the terminal output for details."**
- **Right-click → "Install from directory…"** (or select the pseudo-row and click **Execute**) — pick a folder containing locally built `*.pkg.tar.zst` packages (the build dir is the default start point) and they are installed with an elevated `pacman -U` using absolute paths.

kernel-manager never does the post-install work itself: the distro's alpm hooks (NVIDIA DKMS rebuild, `kernel-install` + dracut or mkinitcpio, bootloader/BLS entry) run inside the pacman transaction. After a successful install, reboot and select the new kernel — right-click any row and choose **"Show boot instructions"** for numbered, bootloader-specific steps (GRUB, systemd-boot, or UKI).

### Driver gate (GPU conflict detection)

Before *any* install (Execute, pre-compiled install, directory install, or post-build install), the gate checks NVIDIA packaging and acts:

- **No NVIDIA hardware, or a compatible DKMS driver + headers already in place** — proceeds silently.
- **A precompiled driver is installed** (`nvidia`, `nvidia-open`, `nvidia-lts`, `nvidia-open-lts`) — it is built only for your distro's kernel, so the custom kernel would have no GPU acceleration (and some systems fail to boot without a display driver). The gate offers a **one-click migration** to the matching DKMS driver — `nvidia-open-dkms` for Turing-or-newer GPUs, `nvidia-dkms` for older or unknown ones — plus the kernel's `-headers`, via `pacman -S --needed --asexplicit` (idempotent; pacman removes the precompiled package). The result is verified against the local DB and the kernel's build directory.
- **DKMS driver present but headers missing** — the headers are noted and installed alongside the kernel in the same transaction.
- **DKMS driver present but headers unavailable from any repo** — you can proceed (with the warning) or cancel.

Declining a fix or a failed migration shows the persistent status-bar banner *"Precompiled nvidia driver detected — custom kernel will NOT have GPU acceleration"* for the session; a successful migration clears it. The gate re-evaluates fresh on every install attempt.

### Remove a Kernel

Uncheck an installed row and click **Execute**. The kernel is removed with `pacman -Rsn` (elevated), which also removes its installed companions — `-headers`, and any of `zfs` / `nvidia` / `nvidia-open` module packages paired with it — plus dependencies no longer needed. The row disappears from the list on the automatic re-scan.

### Configure a Kernel (custom build)

Select a row, then click **Configure** (or right-click → **"Build custom \<name\>"**). A progress dialog clones the kernel's build source (each curated kernel maps to its own source; e.g. `linux-tkg` maps to the TKG git repository), then the **Kernel Manager Configure** window opens with two tabs:

**Options** — the set visible depends on what that kernel's build system supports:

| Control | Values |
|---|---|
| Kernel source | known package list, or a custom git URL / AUR package name |
| Custom package name | default `$pkgbase-custom` (renames the built package) |
| CPU compiler optimizations | `manual`, `native`, `generic v1–v4`, `zen4` |
| LTO | `none`, `full`, `thin` (+ `thin-dist` where supported) |
| Running tick rate | 1000 / 750 / 600 / 500 / 300 / 250 / 100 Hz |
| Tickless mode | `full`, `idle`, `periodic` |
| Preemption | `full`, `lazy`, `voluntary` (+ `none` where supported) |
| Transparent Hugepages | `always`, `madvise` |
| Checkboxes | KBUILD_CFLAGS `-O3`, performance governor as default, TCP_CONG_BBR3, enable custom config, tweak via `nconfig`, tweak via `xconfig`, use modprobed-db, use the current kernel's config, build the ZFS module, build the open NVIDIA module, include `vmlinux` debug symbols |

**Save** writes the current options to a file and **Load** restores them (the file is version-checked).

The **Patches** tab lets you add local patch files or remote patches fetched by URL before the build.

Click **Build kernel**: `makepkg` runs as your user (no root) with its output streamed live to a terminal; a missing GPG signing key is imported automatically and the build retried once. Every build also writes a log to `$XDG_CACHE_HOME/kernel-manager/build-<timestamp>-<pid>.log` (default `~/.cache/kernel-manager/`). On success the driver gate runs, the app asks *"Do you want to install build packages?"*, installs the built packages with an elevated `pacman -U`, reports the two-state result, and offers the boot instructions.

### sched-ext (BPF) schedulers

If your booted kernel exposes `/sys/kernel/sched_ext/state` and the app was built with `scx-manager` support (an optional feature; `scx-manager` ships in the CachyOS repo and is on the AUR), a **"sched-ext scheduler config"** button appears. It opens the scx-manager interface to enable and configure sched-ext (BPF) schedulers. The information card also reports whether the booted kernel has `SCHED_CLASS_EXT` compiled in.

### Refresh

**Refresh** (between Configure and Close) re-parses the pacman databases, re-fetches the kernel list, re-renders the information card, and **purges stale rows** — ghost entries for built or folder-installed kernels that are no longer present, and curated info-rows whose repo is disabled and not installed. It is a no-op while a transaction or a Configure clone is in flight, and it is never destructive to system state (the list is read-only data; no package or config file is touched). The list also refreshes automatically after every Execute transaction.

## Active Kernel Telemetry

The information card shows 15 values about the **booted** kernel, extracted entirely from local files (all read silently; any missing fact degrades to `—`):

| Field | Source |
|---|---|
| Release | `/proc/sys/kernel/osrelease` |
| Compiler | parsed from `/proc/version` (e.g. `clang 18`, `gcc 14`) |
| Arch (CPU target) | kernel config: `Native`, `Family Optimized [CONFIG_M…]`, `Generic`, or `Custom` |
| Build date | `/proc/version`, compact `Mon DD, YYYY HH:MM` |
| ISA level | kernel config (e.g. `x86-64-v3`) |
| ISA validation | one loaded module disassembled and checked for BMI2/AVX2 instructions (`BMI2/AVX2 verified`) |
| LTO | kernel config: `Full LTO` / `Thin LTO` / `None` |
| Optimization | kernel config: `-O3` / `-O2` |
| Tick rate | kernel config `CONFIG_HZ` (e.g. `1000 Hz`) |
| sched_ext | kernel config `CONFIG_SCHED_CLASS_EXT`: `Enabled` / `Disabled` |
| Preemption | live `/sys/kernel/debug/sched/preempt` (root-only; falls back to the config: `Dynamic` / `Full` / `Voluntary`) |
| MGLRU | `/sys/kernel/mm/lru_gen/enabled` |
| THP | `/sys/kernel/mm/transparent_hugepage/enabled` |
| TCP congestion | `sysctl net.ipv4.tcp_congestion_control` |
| Clocksource | `/sys/devices/system/clocksource/clocksource0/current_clocksource` |

The kernel config comes from `zcat /proc/config.gz` when available, else `/boot/config-<release>`. Values are color-coded: **green** = active/verified, **amber** = working but degraded (e.g. `Generic`/`Custom` CPU target, `-O2`, `Thin LTO`, lazy/voluntary preemption), **gray** = off or unknown. Long values are elided with the full text one hover away.

## Multi-language Support

The interface is translated into 16 languages (Bulgarian, Catalan, Czech, German, Spanish, Italian, Japanese, Korean, Dutch, Polish, Russian, Slovak, Swedish, Turkish, Ukrainian, Simplified Chinese). There is no in-app switch: the UI follows the **system locale** at startup. To use a different language, set the locale before launching, e.g.:

```sh
LANG=de_DE.UTF-8 kernel-manager
```

## Kernel Variants

The 21 maintained variants, each mapped to its own build source:

### Official Arch

| Kernel | Source | Description |
|---|---|---|
| `linux` | core | Upstream stable — the general-purpose reference for desktops and servers |
| `linux-lts` | core | Long-term support branch for stability-critical systems |
| `linux-zen` | extra | Low-latency desktop kernel (Zen patchset: tuned schedulers, memory, high-frequency timers) |
| `linux-hardened` | extra | Security-hardened with exploit-mitigation patches |
| `linux-rt` | extra | PREEMPT_RT real-time — deterministic, bounded latency |
| `linux-rt-lts` | extra | PREEMPT_RT on an LTS base |

### CachyOS

| Kernel | Source | Description |
|---|---|---|
| `linux-cachyos` | cachyos | Heavily optimized mainline — Clang ThinLTO, AutoFDO, Propeller, BORE scheduler |
| `linux-cachyos-bore` | cachyos | Dedicated BORE scheduler variant for interactive latency and frame pacing |
| `linux-cachyos-rt-bore` | cachyos | PREEMPT_RT + BORE — low-jitter real-time audio and simulation |
| `linux-cachyos-lts` | cachyos | LTS base with the CachyOS patchset |
| `linux-cachyos-server` | cachyos | Server-oriented: lazy preemption and high-throughput tuning |
| `linux-cachyos-deckify` | cachyos | Optimized for handheld gaming consoles (Steam Deck, MSI Claw) |
| `linux-cachyos-bmq` | cachyos | BMQ bitmap-queue scheduler variant |

### Community

| Kernel | Source | Description |
|---|---|---|
| `linux-mainline` | chaotic-aur | Tracks Linus' master branch and release candidates |
| `linux-tkg` | git (build-only) | TKG configurable build framework — choose scheduler, compiler, and patches; no pre-compiled package |
| `linux-xanmod` | chaotic-aur | Performance suite: memory tuning, high-frequency ticks, BBRv3/CAKE queueing |
| `linux-xanmod-edge` | chaotic-aur | XanMod on a mainline (edge) base |
| `linux-xanmod-lts` | chaotic-aur | XanMod on an LTS base |
| `linux-xanmod-rt` | chaotic-aur | XanMod + PREEMPT_RT |
| `linux-lqx` | liquorix | Liquorix — low-latency audio, multimedia, and preemption tuning |
| `linux-clear` | chaotic-aur | Intel Clear Linux performance and power-management patchset |

## Troubleshooting

- **"No kernels found! Please run `pacman -Sy`"** — the package databases are stale or empty. Run `sudo pacman -Sy`, then reopen or click **Refresh**.
- **Lock banner ("A pacman instance is already running…")** — another pacman (or this app's own terminal) holds the DB lock. Wait for it to exit; the banner clears within 2 seconds.
- **Locked row (lock glyph) / version shows `— (repo not enabled)`** — the kernel's repo is not in `/etc/pacman.conf`. Right-click the row and choose **"Add repo '…'"** (backs up the file, appends the section, runs `pacman -Sy` as root — supported: `cachyos`, `chaotic-aur`, `liquorix`), or edit `pacman.conf` yourself and **Refresh**.
- **Version shows a bare `—`** — the repo is enabled but the package is missing from its DB: run `sudo pacman -Sy` and **Refresh**.
- **Install failed (rc≠0)** — read the terminal output (the source of truth). For custom builds, check the build log in `$XDG_CACHE_HOME/kernel-manager/` (default `~/.cache/kernel-manager/`); a missing GPG key is imported and retried automatically, otherwise import it manually (`sudo pacman -S <pkg>` after adding the maintainer's key). AUR pre-compiled installs need `paru` installed; without it the build path takes over.
- **GPU has no acceleration after installing a custom kernel** — you are on a precompiled NVIDIA driver. Re-run the install and accept the one-click DKMS migration, or migrate manually: `sudo pacman -S --needed --asexplicit nvidia-open-dkms <kernel>-headers` (use `nvidia-dkms` on pre-Turing GPUs).
- **The driver warning banner stays in the status bar** — it documents a declined or failed fix: the custom kernel will have no GPU acceleration until you run the migration or provide the headers.
- **Permission prompts** — escalated operations ask for polkit `auth_admin` authentication. If `pkexec` is missing or the policy not loaded, run `sudo systemctl restart polkit` (or reinstall the package, which reloads the policy).
- **New kernel not in the boot menu** — right-click the row → **Show boot instructions**; on GRUB systems run `sudo grub-mkconfig -o /boot/grub/grub.cfg`.
- **The "sched-ext scheduler config" button is missing** — it appears only when the booted kernel supports sched_ext *and* the app was built with `scx-manager` support; install `scx-manager` (CachyOS repo or AUR) and rebuild/reinstall.

## Privacy & Security

- **No data is collected.** Everything the app reads is local: `/proc`, `/sys`, the pacman databases, and `/etc/os-release`. There is no telemetry, no phone-home, and no network activity except what the operations themselves require — pacman repo syncs, `git` clones of build sources, and `paru`/AUR lookups for AUR builds.
- **Privilege escalation is limited and auditable.** The app runs as your user. Root is used only through a polkit action (`org.archlinux.kernel-manager.pkexec.policy.run-root-terminal`, `auth_admin`) that execs the installed `rootshell.sh` helper in a visible terminal — never through setuid binaries or a background root process. Escalated commands are: repository install (`pacman -S --needed`), removal (`pacman -Rsn`), local-package install (`pacman -U`), add-repo (`repo_add.sh`), and driver migration. Custom kernel *builds* (`makepkg`) run as your user with no root.
- **Shell safety.** Commands executed in the terminal are fixed, curated strings; any package, module, or path name is restricted to its safe character class before use, so user-visible text never reaches a shell.
- **Transparency.** Every privileged operation runs in a terminal you can watch, its real exit code decides the green/red result, and builds keep a persistent log for post-mortem analysis.
