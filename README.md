# kernel-manager
Simple kernel manager.

That kernel manager is only supports kernels from any arch based repos.
###### Note: does support kernels from AUR (requires paru & awk installed). **disabled by default**.

**Note on AUR builds:** kernel builds from AUR sources validate the source
checksums declared in the PKGBUILD (`--skipchecksums` is *not* used). If an
AUR package's source URL changes but its published checksums no longer
match, the build will fail by design — the AUR package needs to be updated
(re-cloned) before it can be built again.

Requirements
------------
* C++23 feature required (tested with GCC 14.1.1 and Clang 18)
Any compiler which support C++23 standard should work.

######
## Installing from the AUR

The project is packaged for the AUR in two variants:

* `kernel-manager` — stable release package
* `kernel-manager-git` — tracks the `main` branch

```sh
# with any AUR helper (yay, paru, trizen, ...)
yay -S kernel-manager
# or the git-based variant
yay -S kernel-manager-git
```

## Installing from source

This is tested on Arch Linux, but *any* recent Arch Linux based system with a latest C++23 compiler should do:

```sh
sudo pacman -S \
    base-devel cmake pkg-config make qt6-base qt6-tools polkit-qt6 python rust
```

(`rust` is required for the Corrosion/cxx config bridge; `git` ships with
`base-devel`.)

### Cloning the source code
```sh
git clone https://github.com/MadGoatHaz/kernel-manager.git
cd kernel-manager
```

### Building and Configuring

The project is built with **CMake only** (there is no other build system).
`configure.sh` is the CMake entry point: it runs `cmake` and generates
`build.sh`, which drives `cmake --build` (if you intend to install it
globally, you might also want `--prefix=/usr`):

```sh
./configure.sh --prefix=/usr/local
./build.sh
```

The equivalent raw CMake invocation:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build -j$(nproc)
```

### Optional: sched-ext (scx-manager) support

Managing sched-ext (BPF) schedulers via scx-manager is **optional**. CMake
probes for `scxctl-ui` at configure time:

* **default (no flag):** auto — enabled if `scx-manager` is installed,
  disabled otherwise (re-evaluated on every configure)
* `-DWITH_SCX_MANAGER=ON`: force the feature on (requires `scx-manager` to
  be installed; a missing package is a hard error)
* `-DWITH_SCX_MANAGER=OFF`: force the feature off

```sh
cmake -S . -B build -DWITH_SCX_MANAGER=OFF   # generic-Arch build, no scx
cmake --build build -j$(nproc)
```

`scx-manager` ships in the cachyOS repo and is available via the AUR on
generic Arch; it is declared as an optional dependency (`optdepends`) in
the package builds.

### Libraries used in this project

* [Qt](https://www.qt.io) used for GUI.
* [A modern formatting library](https://github.com/fmtlib/fmt) used for formatting strings, output and logging (fetched via CPM).
* [frozen](https://github.com/serge-sans-paille/frozen) and [Corrosion](https://github.com/corrosion-rs/corrosion)/[cxx](https://github.com/dtolnay/cxx) for the compile-time option maps and the Rust config bridge.
