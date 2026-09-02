# kernel-manager

A modern, polished kernel manager for Arch Linux — install, build, and switch between 21 maintained kernels, with bootloader-aware boot guidance.

[![Build](https://github.com/MadGoatHaz/kernel-manager/actions/workflows/build.yml/badge.svg)](https://github.com/MadGoatHaz/kernel-manager/actions/workflows/build.yml)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/License-GPL--3.0--or--later-blue)](LICENSE)

kernel-manager is a Qt6 GUI for managing Linux kernels on Arch Linux and Arch-based systems. Browse the full landscape of maintained kernel variants, install a pre-compiled kernel with a single action, or build a custom kernel from source — and get exact, bootloader-aware instructions for selecting it at your next boot.

## Features

- **Browse 21 maintained kernel variants** — official Arch, CachyOS, and community kernels (XanMod, TKG, Liquorix, Clear, mainline) — each with a description tooltip. The list is complete: kernels whose repository is not enabled appear as clearly-marked, non-selectable info-rows with the reason, and an **Installed** column truthfully reports which kernels are installed on the system (✓/—).
- **Install pre-compiled kernels with one action** — from `[core]`/`[extra]`, the CachyOS repositories, `chaotic-aur`, or the AUR (via `paru`). kernel-manager handles the package install, initramfs regeneration (`mkinitcpio`), and bootloader refresh for you.
- **Install kernels from a directory** — right-click the "Install from directory…" row in the kernel list, pick the folder containing your built `*.pkg.tar.zst` package(s), and kernel-manager installs them (`pacman -U`) with the same initramfs + bootloader refresh. Kernels installed from local files appear in the list as `local/<name>` entries — with a real version and a remove checkbox — so you can uninstall them from the list the same way.
- **Build custom kernels from source** — pick a known package in the source dropdown (or enter any git URL), tweak compile options and patches, and compile with `makepkg`. Build options are data-driven per kernel family: the full 17-option CachyOS suite for `linux-cachyos` and its 6 sibling flavors, CPU-optimization + modprobed-db for the 4 XanMod flavors, and modprobed-db for Liquorix — shown only where the selected source's PKGBUILD actually consumes them, so official Arch, mainline, Clear, and TKG intentionally show none.
- **Choose where builds happen** — a bottom-left build-directory selector (path label + "Browse…") lets you point kernel-manager at any folder for its build trees; the choice persists across sessions (default: `~/.cache/kernel-manager/pkgbuilds`).
- **Bootloader-aware boot instructions** — the app detects which boot loader is in use (Unified Kernel Image, systemd-boot, GRUB, or unknown) and shows numbered steps for selecting the new kernel at next boot, including how to make it the default.
- **Right-click context menu on every kernel** — *Install pre-compiled* (enabled only when the kernel is actually installable from its repository), *Build custom*, or *Show boot instructions*.
- **Unified privilege escalation via polkit** — a single `auth_admin` action (no `sudo` required).
- **Polished desktop integration** — a dedicated app icon, reliably shown on both the window titlebar and the system taskbar (the Wayland "W" placeholder is gone), plus translation support.

## Supported kernels

kernel-manager tracks 21 maintained Arch kernel variants, each mapped to its own build source.

### Official Arch kernels

| Kernel | Description |
|--------|-------------|
| `linux` | Upstream stable — the default reference kernel |
| `linux-lts` | Long-term support (LTS) branch for stability-critical systems |
| `linux-zen` | Low-latency tuning for desktops and gaming (Zen patchset) |
| `linux-hardened` | Security-hardened with exploit mitigations |
| `linux-rt` | PREEMPT_RT real-time |
| `linux-rt-lts` | PREEMPT_RT on an LTS base |

### CachyOS kernels

| Kernel | Description |
|--------|-------------|
| `linux-cachyos` | Optimized mainline — Clang ThinLTO, AutoFDO/Propeller, BORE scheduler |
| `linux-cachyos-bore` | Tuned for interactive latency and frame pacing (BORE scheduler) |
| `linux-cachyos-rt-bore` | PREEMPT_RT determinism combined with BORE |
| `linux-cachyos-lts` | LTS base with the CachyOS patchset |
| `linux-cachyos-server` | Server-oriented: lazy preemption, high-throughput configuration |
| `linux-cachyos-deckify` | Handheld gaming (Steam Deck, MSI Claw, and similar) |
| `linux-cachyos-bmq` | BMQ bitmap-queue scheduler |

### Community kernels

| Kernel | Description |
|--------|-------------|
| `linux-mainline` | Tracks Linus' master branch and release candidates |
| `linux-tkg` | Configurable build framework — choose scheduler, LTO mode, and patches |
| `linux-xanmod` | XanMod performance suite for low-latency desktops |
| `linux-xanmod-edge` | XanMod on bleeding-edge mainline |
| `linux-xanmod-lts` | XanMod on an LTS base |
| `linux-xanmod-rt` | XanMod with PREEMPT_RT |
| `linux-lqx` | Liquorix — low-latency audio and multimedia |
| `linux-clear` | Intel Clear Linux performance patchset |

## How it works

kernel-manager offers three complementary paths:

- **Install pre-compiled** — one action installs the kernel package from your configured pacman repositories (or the AUR, via `paru`), regenerates the initramfs with `mkinitcpio`, and refreshes the bootloader. The kernel list is complete: kernels whose repository is not enabled appear as clearly-marked, non-selectable info-rows with the reason (repository not enabled vs. package not in the repository), the **Installed** column truthfully reports which kernels are installed on this system (pacman local DB, ✓/—), and the context menu's *Install pre-compiled* action reflects real availability.
- **Install from a directory** — for packages built anywhere (your own `makepkg` run, a download, another machine): right-click the "Install from directory…" row, pick the folder with the `*.pkg.tar.zst` package(s), and kernel-manager installs them with `pacman -U`, regenerates the initramfs, and refreshes the bootloader. The kernel then appears in the list as a `local/<name>` entry, removable by unchecking it.
- **Build custom** — the configure flow clones the selected source (an AUR package or any git URL) into the build directory of your choice (bottom-left selector, persisted), lets you adjust compile options and patches, and compiles the kernel with `makepkg` (missing GPG signing keys are imported automatically).

After a kernel is installed or built, kernel-manager detects your boot loader (in priority order: Unified Kernel Image → systemd-boot → GRUB → unknown) and shows numbered steps for selecting the new kernel at your next boot — including the mandatory `/boot/vmlinuz-<pkg>` / `mkinitcpio` note — and for making it the default entry.

Privileged operations (kernel install and removal, custom-build installation) are escalated through a single polkit `auth_admin` action via `pkexec`; no `sudo` is required.

### Note on AUR builds

Kernel builds from AUR sources validate the source checksums declared in the PKGBUILD (`--skipchecksums` is *not* used). If an AUR package's source URL changes but its published checksums no longer match, the build will fail by design — the AUR package needs to be updated (re-cloned) before it can be built again.

## Requirements

- C++23 compiler (tested with GCC 14+ and Clang 18)
- Qt6 (Widgets, Concurrent, LinguistTools)
- `libalpm` (pacman) ≥ 13.0.0 and `glib` ≥ 2.72.1
- `polkit-qt6` (build-time CMake configuration)
- Rust — for the Corrosion/cxx config bridge
- CMake ≥ 3.20 and Python 3 (for compile-option code generation)

Install the build dependencies on Arch Linux:

```sh
sudo pacman -S \
    base-devel cmake pkg-config make qt6-base qt6-tools polkit-qt6 python rust
```

(`rust` is required for the Corrosion/cxx config bridge; `git` ships with `base-devel`.)

## Installing

### From the AUR

The project is packaged for the AUR in two variants:

- `kernel-manager` — stable release package
- `kernel-manager-git` — tracks the `main` branch

```sh
# with any AUR helper (yay, paru, trizen, ...)
yay -S kernel-manager
# or the git-based variant
yay -S kernel-manager-git
```

### From source

This is tested on Arch Linux, but *any* recent Arch-based system with a current C++23 compiler should work:

```sh
git clone https://github.com/MadGoatHaz/kernel-manager.git
cd kernel-manager
```

The project is built with **CMake only** (there is no other build system). Configure and build:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Run the built binary:

```sh
./build/kernel-manager
```

Optionally install it for your user (no `sudo` required):

```sh
cmake --install build --prefix ~/.local
```

`configure.sh` is an alternative entry point: it wraps the CMake invocation and generates `build.sh`, which drives `cmake --build` (if you intend to install it globally, you might also want `--prefix=/usr`):

```sh
./configure.sh --prefix=/usr/local
./build.sh
```

### Optional: sched-ext (scx-manager) support

Managing sched-ext (BPF) schedulers via scx-manager is **optional**. CMake probes for `scxctl-ui` at configure time:

- **default (no flag):** auto — enabled if `scx-manager` is installed, disabled otherwise (re-evaluated on every configure)
- `-DWITH_SCX_MANAGER=ON`: force the feature on (requires `scx-manager` to be installed; a missing package is a hard error)
- `-DWITH_SCX_MANAGER=OFF`: force the feature off

```sh
cmake -S . -B build -DWITH_SCX_MANAGER=OFF   # generic-Arch build, no scx
cmake --build build -j$(nproc)
```

`scx-manager` ships in the CachyOS repository and is available via the AUR on generic Arch; it is declared as an optional dependency (`optdepends`) in the package builds.

## Libraries used

- [Qt](https://www.qt.io) — used for the GUI.
- [fmt](https://github.com/fmtlib/fmt) — string formatting, output, and logging (fetched via CPM).
- [frozen](https://github.com/serge-sans-paille/frozen) — compile-time option maps.
- [Corrosion](https://github.com/corrosion-rs/corrosion) / [cxx](https://github.com/dtolnay/cxx) — the Rust config bridge.

## Contributing

Contributions are welcome! Report bugs and suggest features via the [issue tracker](https://github.com/MadGoatHaz/kernel-manager/issues), or open a pull request against the `main` branch. The project ships a standalone unit-test suite under `tests/` (run via the accompanying `run_*.sh` scripts) — please keep the CMake build free of new compiler warnings and extend the tests to cover any new functionality. See [Changelog.md](Changelog.md) for the project's release history.

## License

kernel-manager is licensed under the [GNU General Public License v3.0 or later](LICENSE).
