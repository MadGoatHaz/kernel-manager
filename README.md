# kernel-manager

Build and install custom Arch Linux kernels from a polished Qt6 GUI — 21 kernel variants, distribution-aware, with honest exit-code-backed install results.

[![Build](https://github.com/MadGoatHaz/kernel-manager/actions/workflows/build.yml/badge.svg)](https://github.com/MadGoatHaz/kernel-manager/actions/workflows/build.yml)
[![Checks](https://github.com/MadGoatHaz/kernel-manager/actions/workflows/checks.yml/badge.svg)](https://github.com/MadGoatHaz/kernel-manager/actions/workflows/checks.yml)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/License-GPL--3.0--or--later-blue)](LICENSE)

kernel-manager is a Qt6 desktop app for managing Linux kernels on Arch Linux and Arch-based systems. Install a pre-compiled kernel with a single action, build a custom kernel from source with per-kernel options, or install packages straight from a directory — with bootloader-aware boot guidance and a clear pass/fail result on every install.

## Screenshots

_Screenshot coming soon._

## Features

- **Build custom kernels** — from AUR PKGBUILDs with configurable, per-kernel build options (the full 17-option CachyOS suite, CPU-optimization + modprobed-db for XanMod, and more), compiled with `makepkg`.
- **21 supported kernel variants** — official Arch, CachyOS, and community kernels (XanMod, TKG, Liquorix, Clear, mainline), each mapped to its own build source.
- **One-click install** — the app runs a single `pacman` install and leaves the post-install work (NVIDIA DKMS, initramfs regeneration, bootloader entry) to the distro's own alpm hooks.
- **Distribution-aware** — detects the distro family from `/etc/os-release` (Arch, EndeavourOS, Manjaro, CachyOS, Garuda, and other Arch-based systems) and adapts the initramfs tool and BLS path accordingly.
- **Clear install feedback** — every install reports a two-state result: **Installed** (green) or **Failed** (red, with the real exit code). The terminal output is the source of truth — no silent skips.
- **Optional scx-manager integration** — manage sched-ext (BPF) schedulers when `scx-manager` is installed.
- **Version visible in the UI** — the running version is shown in the window title and the status bar.

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

Pick a kernel and its build options — kernel-manager clones the AUR source, builds it with `makepkg`, and installs it with a single `pacman -U`. The distro's own post-transaction hooks (NVIDIA DKMS, `kernel-install`/dracut) then rebuild drivers, regenerate the initramfs, and create the bootloader entry. kernel-manager reports success or failure with the real exit code — no silent skips and no ambiguous "probably fine" states — and a bootloader-aware dialog walks you through selecting the new kernel at your next start.

## Requirements

- A recent Arch Linux or Arch-based distribution (EndeavourOS, CachyOS, Manjaro, Garuda, and others — see the distribution-aware behavior above)
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

## Development

**Build from source** (see [Installing](#from-source)):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/kernel-manager
```

**Run the tests** — the project ships a standalone unit-test suite of 15 harnesses under `tests/` (one `run_*.sh` driver each), which compile the relevant sources with the project's full warning set and assert against the real APIs. The full suite is expected to pass with zero compiler warnings:

```sh
for t in tests/run_*.sh; do bash "$t"; done
```

An offscreen smoke launch (`QT_QPA_PLATFORM=offscreen ./build/kernel-manager`) should stay alive with no output.

**Project structure:**

- `src/` — the C++23 / Qt6 application (main window, configure dialog, install engine, bootloader + distro detection, terminal helpers)
- `config-option-lib/` — the Rust config-option bridge (built into the C++ app via Corrosion / cxx)
- `tests/` — the standalone test harnesses and their `run_*.sh` drivers
- `cmake/` — CMake modules and configuration
- `icons/`, `lang/` — the application icon assets and translation catalogs

**Libraries:** Qt (GUI), fmt (string formatting / logging, via CPM), frozen (compile-time option maps), and Corrosion / cxx (the Rust config bridge).

## Contributing

Contributions are welcome! Report bugs and suggest features via the [issue tracker](https://github.com/MadGoatHaz/kernel-manager/issues), or open a pull request against the `main` branch. The project ships a standalone unit-test suite under `tests/` (run via the accompanying `run_*.sh` scripts) — please keep the CMake build free of new compiler warnings and extend the tests to cover any new functionality. See [Changelog.md](Changelog.md) for the project's release history.

## License

kernel-manager is licensed under the [GNU General Public License v3.0 or later](LICENSE).
