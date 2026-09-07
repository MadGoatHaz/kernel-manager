# kernel-manager — Architecture

kernel-manager is a Qt6 desktop app for **building and installing custom Linux kernels on Arch** (and Arch-based distros). The user picks a kernel from a list (official, CachyOS, and community variants), configures build options, builds it with `makepkg`, and installs it with `pacman`; the **distro's own ALPM hooks** then do the post-install work (NVIDIA DKMS, initramfs, bootloader entry) inside the transaction — the app never runs `grub-mkconfig`/`mkinitcpio` itself. It is written in **C++23** with a Qt6 **Widgets** front end, uses **libalpm** for package-DB access, a small **Rust** crate (via **Corrosion** + **cxx**) for TOML config (de)serialization, and a set of installed **shell helpers** for the privilege-escalated terminal and the build/log/repo steps. The governing design philosophy is **pure, probe-injected modules**: every piece of logic that reads the filesystem or a command is split from its *decision* logic (the decision is a pure function of injected predicates), so each is unit-testable with no live system, and a **thin Qt layer** only wires UI to those modules and runs commands through a **single polkit/pkexec terminal path**.

## Tech Stack
- **Language:** C++23 (primary, `-std=c++23`, `CMAKE_CXX_STANDARD 23` in `cmake/StandardProjectSettings.cmake:17`); Rust (`config-option-lib`, edition 2024) for TOML config via a `cxx` bridge.
- **UI:** Qt6 **Widgets** (`Qt6::Widgets` + `Qt6::Concurrent`; LinguistTools for translations). `Qt6::LinguistTools` builds the `lang/*.ts` → `*.qm` files (`CMakeLists.txt:106-110`).
- **Package DB:** `libalpm` (pacman's library, `>=13.0.0`) and `glib-2.0` (both via `pkg_check_modules`, `CMakeLists.txt:31-40`); `PolkitQt6-1` for the polkit layer (`CMakeLists.txt:61`).
- **Build:** CMake (`>=3.20`) + **CPM** (vendors `fmt` 12.2.0, `frozen`, `Corrosion` v0.6.1, `CMakeLists.txt:42-59`) + Ninja; Corrosion builds the Rust staticlib (`CMakeLists.txt:188-189`).
- **Privilege escalation:** **polkit/pkexec** via a custom `org.archlinux.kernel-manager` policy; a `terminal-helper` sentinel captures the command's **real exit code**.
- **Packaging:** PKGBUILD (AUR) — installs the binary + 6 shell helpers + desktop file + polkit policy + hicolor icons (`CMakeLists.txt:205-288`).
- **Testing:** 17 standalone **shell harnesses** (`tests/run_*.sh`) — no CTest.

## Repository Layout
```
kernel-manager/
├── CMakeLists.txt              # Single build file: targets, CPM deps, Corrosion, install rules
├── cmake/                      # CMake helper modules (CPM, warnings, sanitizers, options)
├── config-option-lib/          # Rust crate: TOML Config parse/serialize (cxx bridge, staticlib)
│   ├── Cargo.toml              #   edition 2024; deps: cxx, serde, toml, anyhow; crate-type staticlib
│   └── src/lib.rs              #   #[cxx::bridge] Config + parse/write fns + round-trip tests
├── src/                        # All C++ sources + shell helpers + codegen
│   ├── main.cpp                # Entry: single-instance lock, translations, MainWindow
│   ├── km-window.{hpp,cpp,ui}  # Main window: kernel tree, telemetry header, install/refresh flows
│   ├── conf-window.{hpp,cpp,ui}# Configure window: options/patches tabs, build runner
│   ├── conf-options-page.{hpp,ui}  # Options tab (source, name, 17 toggles/combos)
│   ├── conf-patches-page.{hpp,ui}  # Patches tab (list widget, .patch source editing)
│   ├── kernel.{hpp,cpp}        # Kernel model + get_kernels() scan + install/remove lists
│   ├── known_kernels.{hpp,cpp} # Curated 21-variant table (source/desc/install data)
│   ├── kernel_info.{hpp,cpp}   # Booted-kernel telemetry extractor (15 facts, probe-injected)
│   ├── driver_gate.{hpp,cpp}   # nvidia DKMS-vs-prebuilt gate (decision table, probe-injected)
│   ├── bootloader.{hpp,cpp}    # Bootloader detection (UKI/systemd-boot/GRUB, probe-injected)
│   ├── boot_instructions.{hpp,cpp} # Post-install boot-selection steps per bootloader
│   ├── alpm_utils.{hpp,cpp}    # libalpm handle + package/repo availability + repo-add
│   ├── install_kernel.{hpp,cpp}# Pre-compiled + directory install (plan/execute/verdict)
│   ├── distro.{hpp,cpp}        # Distro-family detection (/etc/os-release, probe-injected)
│   ├── config-options.{hpp,cpp}# C++ side of the cxx bridge (TOML Config ↔ C++ struct)
│   ├── aur_kernel.{hpp,cpp}    # AUR build-tree path + AUR kernel install (opt-in)
│   ├── utils.{hpp,cpp}         # exec, runCmdTerminal (pkexec), build env, QSettings paths
│   ├── ini.hpp                 # mINI: INI parser used to read /etc/pacman.conf
│   ├── string_utils.hpp        # C++23 ranges string helpers (split/join/replace)
│   ├── mkoptions.py            # Codegen: compile_options.json → build/compile_options.hpp
│   ├── compile_options.json    # Per-kernel build-option variable map (CachyOS/XanMod/…)
│   ├── km_icons.qrc            # Bundled window-icon resource
│   ├── terminal-helper         # Picks a terminal; sentinel launch/rc protocol (real exit code)
│   ├── rootshell.sh            # pkexec root shell (`exec /bin/bash`) for escalated commands
│   ├── build_helper.sh         # makepkg wrapper: streaming log + GPG auto-import + retry
│   ├── repo_add.sh             # Enable a curated pacman repo (backup+append+pacman -Sy)
│   ├── install_logger.sh       # Run one install step, log full output, return its rc
│   └── postinstall_tail.sh     # Post-install tail: driver sync → initramfs → boot-safety verdict
├── packaging/                  # Installed metadata: polkit policy + .desktop file
├── icons/                      # hicolor icon set (org.archlinux.KernelManager.*)
├── img/                        # README screenshots
├── lang/                       # Qt translation sources (.ts)
├── plans/                      # Dev plans: PLAN.md + archive/ (per-release)
├── scripts/                    # format_file.sh (clang-format), auto-changelog.sh
├── tests/                      # 17 run_*.sh harnesses + their test_*.cpp drivers
├── build.sh / configure.sh     # Thin CMake wrappers (configure.sh writes build.sh)
├── Makefile                    # `pretty_format` + `Changelog.md` only (not the build)
├── PKGBUILD / PKGBUILD-git     # AUR package definitions (git- = -git variant, unpublished)
├── km_locale.qrc               # Translation-resource file
├── DEV_LOG.md                  # Active-worker lease board (gitignored, working doc)
├── Work/                       # Gitignored working docs (DEV_LOG, HANDOVER, ENHANCEMENTS, AUR guide)
└── docs/                       # This file
```

## Core Components

### Main Window (`km-window.hpp` / `km-window.cpp`)
The `MainWindow` (`QMainWindow`, `km-window.hpp:117`) is the app's shell. Layout (from `km-window.ui`): an **"Active Kernel Information" telemetry header** (`kernelInfoHeader` QFrame, `km-window.ui:28`) → a caption → a hidden **pacman-lock banner** (`km-window.ui:52`) → the **kernel list** (`treeKernels` QTreeWidget, 6 columns: Choose / PkgName / Version / Category / Installed / Immutable, `km-window.ui:75-118`) → a bottom row with the **build-dir label + Browse** and the **sched-ext / Configure / Refresh / Close / Execute** buttons (`km-window.ui:121-202`).
- **Key methods:** `build_kernel_info_header()` (`km-window.cpp:885`) renders the telemetry grid; `init_kernels()` (`km-window.cpp:1596`) re-parses alpm + rebuilds the tree; `on_execute()` (`km-window.cpp:1741`) starts the install/remove worker; `on_refresh()` (`km-window.cpp:1662`) → `purge_stale_rows()` (`km-window.cpp:1708`); `on_configure()` (`km-window.cpp:1116`) clones the build repo and opens `ConfWindow`; `on_kernel_context_menu()` (`km-window.cpp:1157`) drives the per-row actions.
- **Telemetry header:** one `kernel_info::extract_kernel_info()` per call into a **single 4×4 `QGridLayout`** (30 labels = 15 keys + 15 values; row 0 = Release spanning cols 0–1 in a +2 pt bold hero font, Compiler at (0,2), Arch at (0,3); rows 1–3 = the 12 remaining params). Idempotent — a repeat tears down and re-renders byte-identically.
- **Signals/slots (ctor, `km-window.cpp:727-805`):** button clicks → `on_execute`/`on_configure`/`on_refresh`/`on_cancel`/`on_browse_build_dir`/`on_schedext_config`; tree `itemChanged`/`customContextMenuRequested`/`double-click` → `item_changed`/`on_kernel_context_menu`/`check_uncheck_item`; a 2 s `QTimer` → `update_pacman_lock_banner` (`km-window.cpp:632`); `QFutureWatcher::finished` → show `ConfWindow` after the background clone.
- **Worker:** a `QThread` + `Work` functor (`km-window.cpp:639`) runs `install_packages`/`remove_packages` (`km-window.cpp:80-103`) → `Kernel::commit_transaction()` (pacman via `runCmdTerminal`), then re-parses alpm and, if state changed, queues `init_kernels` back on the main thread (`km-window.cpp:664-696`). `m_running`/`m_mutex`/`m_cv` serialize the transaction.

### Kernel Model (`kernel.{hpp,cpp}`, `known_kernels.{hpp,cpp}`)
`Kernel` (`kernel.hpp:30`) wraps one alpm package + its `-headers` + optional module packages (zfs/nvidia/nvidia-open) and its repo. A row is either **LIVE** (`m_pkg != nullptr`: a repo/AUR package, installable/removable) or a **curated INFO-row** (`m_pkg == nullptr`: display-only, `version()` = "—", install/remove guard out). `Kernel::get_kernels()` (`kernel.cpp:275`) builds the list in **four passes**:
1. **Sync DBs:** search every enabled repo for the `linux*-headers` needle, pair each with its kernel + module packages (data-driven via `kKernelModuleTable`, `kernel.cpp:97`).
2. **AUR** (opt-in `ENABLE_AUR_KERNELS`, OFF by default): `paru --aur -Sl` for AUR header packages (`kernel.cpp:333-378`).
3. **Curated info-rows:** `km::known_kernels()` entries with a pre-compiled package not already listed (`kernel.cpp:380-402`).
4. **Local-only:** installed kernels absent from every enabled sync DB, surfaced as `local/<name>` (`kernel.cpp:419-461`).

`known_kernels.cpp` (`km` namespace) is the **single source of truth** mapping a kernel to its build source, description, and pre-compiled install data. **21 curated variants** (`known_kernels.cpp:55-237`): `linux`, `linux-lts`, `linux-zen`, `linux-hardened`, `linux-rt`, `linux-rt-lts` (Arch core/extra); `linux-cachyos`, `-bore`, `-rt-bore`, `-lts`, `-server`, `-deckify`, `-bmq` (CachyOS); `linux-mainline`, `-tkg`, `-xanmod`, `-xanmod-edge`, `-xanmod-lts`, `-xanmod-rt`, `-clear` (chaotic-aur); `linux-lqx` (liquorix). Lookup is prefix-tolerant (`kernel_name_from_raw`, `known_kernels.cpp:307`); `find_kernel`/`default_source_for`/`description_for`/`known_sources`/`is_installable` (`known_kernels.hpp:57-79`) back the Configure source dropdown, tooltips, and the context-menu install action.

### Kernel Telemetry (`kernel_info.{hpp,cpp}`)
`kernel_info::extract_kernel_info()` (`kernel_info.hpp:80`) produces the **15 display-ready facts** the header renders: release, build date, compiler, target arch, ISA level, instruction validation, LTO status, optimization flag, tick rate, sched_ext, preemption model, mGLRU, THP, TCP congestion, clocksource. Detection reads `/proc`, `/sys`, `/boot`, `modinfo`, and `llvm-objdump` — but **all behind an injectable `KernelInfoProbe`** (`kernel_info.hpp:42`); an unbound predicate degrades a field to "" (no crash). The real overload wires the std-only system reads; the pure overload is what the k20 harness drives with fakes.

### Configuration (`conf-window`, `config-options`, `config-option-lib`)
`ConfWindow` (`conf-window.hpp:71`, application-modal) has two tabs — **Options** (`conf-options-page.ui`) and **Patches** (`conf-patches-page.ui`). The Options page exposes: the **build source** combo (curated sources + "Custom URL…") + custom package name + a **17-item suite** of toggles/combos (custom/nconfig/xconfig config, modprobed-db, use-current-config, -O3, performance governor, TCP BBR3, tick rate, tickless, preemption, hugepages, CPU target, LTO, built-in ZFS / open-NVIDIA / debug symbols). **Build flow** (`ConfWindow::on_execute`, `conf-window.cpp:1506`): `sync_build_source` → `utils::prepare_build_environment()` (clone/fetch the source repo, `utils.cpp`) → `refresh_flavors()` → rewrite the PKGBUILD (insert `.patch` sources, set custom name) → `run_cmd_async` (`conf-window.cpp:709`) launches `build_helper.sh -scf --cleanbuild` in a `QProcess`. Build completion is detected via the `.done-status` marker + a bounded poll (`handle_build_done`, `conf-window.cpp:778`), which then runs the driver gate and the post-build `pacman -U`.
- **Config persistence:** options are (de)serialized as TOML through the **Rust crate** — `ConfigOptions::parse_from_file`/`write_config_file` (`config-options.cpp:43,79`) call the `cxx` bridge (`::km::Config`, `config-option-lib/src/lib.rs:22-56`) which uses `serde`/`toml`. **How config reaches the build:** the option values map to per-kernel PKGBUILD variables via `compile_options.json` → `mkoptions.py` → generated `build/compile_options.hpp` (e.g. `hardly_check` → `_cc_harder` for CachyOS), applied during the PKGBUILD rewrite.

### Driver Gate (`driver_gate.{hpp,cpp}`)
A Qt-free pre-flight that decides **NVIDIA DKMS-vs-prebuilt packaging** before any build/install. `driver_gate::evaluate_gate()` (`driver_gate.hpp:120`) returns a `GateVerdict` over the `GateAction` decision table (`PROCEED` / `WARN_MIGRATE` / `ENSURE_HEADERS` / `WARN_ONLY`, `driver_gate.hpp:52`): it classifies the GPU generation from `lspci` (`classify_gpu`, `driver_gate.hpp:108`) — Turing-and-newer → `nvidia-open-dkms`, pre-Turing → `nvidia-dkms` — and inspects installed drivers, the build dir, and sync-DB header availability, all through an injectable `GateProbe` (`driver_gate.hpp:72`). The Qt layer is a single glue method, `run_driver_gate()` (`km-window.cpp:1436`, and the post-build copy in `conf-window`), wired to four triggers: **A** main-window Execute, **B** context-menu pre-compiled install, **C** install-from-directory, **D** post-build. A declined fix shows the persistent status-bar banner (`show_driver_banner`, `km-window.cpp:1535`); `derive_headers_pkg()` (`driver_gate.hpp:101`) builds the companion `-headers` name generically.

### Installation (`install_kernel`, `bootloader`, `boot_instructions`)
`install_kernel` (`install_kernel.hpp`) is the **pre-compiled + directory** install capability, entirely table-driven and reusing the other modules:
- `plan_steps()` (`install_kernel.hpp:76`) → one step each: pacman repo → `pacman -S --needed <pkg>` (escalated); AUR → `paru -S --needed <pkg>` (not escalated); build-only → `build_helper.sh -sicf --cleanbuild`; else empty. Since *simplify-K1* the plan is just the install step — the distro's ALPM hooks do the post-install work inside the transaction.
- `install_kernel()` (`install_kernel.hpp:136`) executes the plan via an injected `CommandRunner` (default `utils::runCmdTerminal`), returns a 2-state `InstallVerdict` (`INSTALL_SUCCESS`/`INSTALL_FAILED`) from the **real exit code**, plus the boot-selection steps.
- `install_from_directory()` (`install_kernel.hpp:279`) installs locally built `*.pkg.tar.zst` (via `list_local_packages`/`read_pkginfo`) with one escalated `pacman -U` of absolute paths, logging through `install_logger.sh` to `~/.cache/kernel-manager/install-<ts>.log`.
`bootloader::detect_bootloader()` (`bootloader.hpp:62`) classifies **UKI > systemd-boot > GRUB > UNKNOWN** from injectable path/command/UKI probes; `boot_instructions::instructions_for()` (`boot_instructions.hpp:37`) turns that into the ordered post-install steps shown in a dialog. **Privilege escalation:** every root command goes through `utils::runCmdTerminal(cmd, escalate)` (`utils.hpp:60`) → `pkexec` → `rootshell.sh` → the polkit `auth_admin` action; `terminal-helper` (`src/terminal-helper`, sentinel at `terminal-helper:84`) runs the command in a subshell and records the **real rc** (the D-Bus terminal-launcher rc is otherwise meaningless), exiting with that rc so the app reports honest success/failure.

### AUR Integration (`aur_kernel`, `repo_add.sh`)
`aur_kernel` (`aur_kernel.hpp:27`) holds the AUR build tree under the user build dir (`<buildDir>/aur_pkgbuilds/<pkg>`) and installs AUR kernels via `detail::install_aur_kernels`. **Repo management** is in `utils::add_repo_to_pacman_conf()` (`alpm_utils.hpp:101`): it guards the input (non-empty, not `aur`, charset `[a-z0-9-]`, must be a curated `install_repo`) then runs `repo_add.sh '<repo>'` (escalated). `repo_add.sh` (`src/repo_add.sh`) is the **last wall** — a static 3-repo allowlist (cachyos / chaotic-aur / liquorix) that backs up `/etc/pacman.conf`, appends the `[section]` + mirror servers, proactively imports/trusts the repo GPG key, runs `pacman -Sy`, and repairs both GPG failure modes (unknown key, untrusted signer). A disabled curated repo's row instead offers an in-app "Add repo" context action.

### Alpm Utilities (`alpm_utils.{hpp,cpp}`)
`utils::parse_alpm()` / `release_alpm()` (`alpm_utils.hpp:36-37`) create/free the libalpm handle (root `/`, libdir `/var/lib/pacman/`, `alpm_utils.hpp:33-34`). Availability probes (all no-root, read-only, repeatable): `is_package_in_sync_db()` (`alpm_utils.hpp:70`, via `alpm_db_get_pkg`), `is_aur_package_available()` (`alpm_utils.hpp:63`, via `paru --aur -Si`), `is_package_available()` (`alpm_utils.hpp:77`, dispatches on repo type), and `is_repo_enabled()` (`alpm_utils.hpp:84`, parses `/etc/pacman.conf` with `ini.hpp`'s mINI). These back the context-menu "Install pre-compiled" enable check (E14) and the info-row install column.

## Data Flow
```
┌─ APP STARTUP ─────────────────────────────────────────────────────────────────┐
│ main.cpp:113  single-instance lock → QApplication → translations → MainWindow │
│   ├─ ctor: build_kernel_info_header()  ← kernel_info::extract_kernel_info()  │
│   │         (booted-kernel telemetry: /proc /sys /boot + modinfo/llvm-objdump)│
│   ├─ ctor: m_kernels = Kernel::get_kernels(alpm)  [member init, km-window.hpp:242]
│   └─ show()                                                                     │
│                                                                                │
│ Kernel::get_kernels()  kernel.cpp:275                                          │
│   pass1 sync DBs "linux*-headers" → kernel+headers+modules (kKernelModuleTable)│
│   pass2 AUR (opt-in)  → paru --aur -Sl                                          │
│   pass3 curated info-rows (km::known_kernels, precompiled, not listed)          │
│   pass4 local-only  → installed but absent from any enabled sync DB             │
│   ⇒ render rows into treeKernels (Choose/PkgName/Version/Category/Installed/Imm)│
└────────────────────────────────────────────────────────────────────────────────┘

┌─ INSTALL (checked rows + Execute) ────────────────────────────────────────────┐
│ on_execute  km-window.cpp:1741                                                │
│   ├─ build GateTarget from installable rows                                    │
│   ├─ run_driver_gate()  ← driver_gate::evaluate_gate()   [trigger A]          │
│   │     PROCEED / WARN_MIGRATE / ENSURE_HEADERS / WARN_ONLY                    │
│   ├─ start worker (QThread)                                                   │
│   └─ worker: install_packages + remove_packages → Kernel::commit_transaction()│
│          → pacman -S / pacman -Rsn  via runCmdTerminal(pkexec)                │
│          → ALPM hooks (70-dkms, 90-kernel) do DKMS+initramfs+boot entry        │
│          → re-parse alpm → QMetaObject::invokeMethod(init_kernels) [auto-refresh]│
│ CONTEXT-MENU "Install pre-compiled": install_kernel()  install_kernel.hpp:136  │
│   plan_steps → pacman -S/paru -S (driver gate [trigger B]) → 2-state verdict  │
└────────────────────────────────────────────────────────────────────────────────┘

┌─ CONFIGURE / CUSTOM BUILD ────────────────────────────────────────────────────┐
│ on_configure  km-window.cpp:1116 → apply_source_for_kernel (km table)          │
│   → QtConcurrent: prepare_build_environment (clone/fetch) + refresh_flavors    │
│   → show ConfWindow                                                           │
│ ConfWindow::on_execute  conf-window.cpp:1506                                   │
│   → rewrite PKGBUILD (patches + custom name + compile_options map)             │
│   → run_cmd_async("build_helper.sh -scf --cleanbuild && touch .done-status")   │
│   → makepkg (streaming log + GPG auto-import)                                 │
│   → .done-status / poll → handle_build_done  conf-window.cpp:778              │
│        → driver gate [trigger D] → post-build pacman -U → boot instructions    │
└────────────────────────────────────────────────────────────────────────────────┘

┌─ REFRESH ─────────────────────────────────────────────────────────────────────┐
│ on_refresh  km-window.cpp:1662 (no-op if a transaction is in flight)          │
│   → init_kernels() (re-parse alpm + re-scan + rebuild tree)                   │
│   → purge_stale_rows()  km-window.cpp:1708 (drop info/local rows no longer    │
│         available or installed; "Install from directory…" row always survives) │
│   → build_kernel_info_header() (idempotent re-extract)                        │
└────────────────────────────────────────────────────────────────────────────────┘

  Escalation path (all root commands):  runCmdTerminal(cmd, true)  →  pkexec
     → rootshell.sh (bash)  →  terminal-helper (picks $TERMINAL, sentinel
     wrapper runs the cmd in a subshell, records real rc, exits with it)
```

## Build System
- **Target:** one `qt_add_executable(kernel-manager, …)` (`CMakeLists.txt:128-150`) listing every `.hpp`/`.cpp`, the generated `compile_options.hpp`, the `.ui` files, and the Qt resources (`km_locale.qrc` + `km_icons.qrc`). AUTOMOC/AUTOUIC/AUTORCC are on (`CMakeLists.txt:83-86`).
- **CPM packages:** `fmt` 12.2.0, `frozen`, `Corrosion` v0.6.1 (`CMakeLists.txt:42-59`) — vendored into `build/_deps`, `EXCLUDE_FROM_ALL`.
- **Rust:** `corrosion_import_crate` + `corrosion_add_cxxbridge` for `config-option-lib` (`CMakeLists.txt:188-189`), linked as `config-option-lib-cxxbridge` (`CMakeLists.txt:191`).
- **Compile-time defines:** `KM_HELPER_DIR` (`/usr/lib/kernel-manager`, `CMakeLists.txt:158-159`), `KM_IGNORE_REPO` (`CMakeLists.txt:163-164`), `APP_VERSION` (`CMakeLists.txt:174`), `WITH_SCX_MANAGER` (optional scx-manager gate, `CMakeLists.txt:71-78,168-170`), `HAVE_ALPM_INSTALLED_DB` (`CMakeLists.txt:96-99`), `ENABLE_AUR_KERNELS` (opt-in, `cmake/StandardProjectSettings.cmake:74`).
- **Codegen:** `mkoptions.py` reads `compile_options.json` → `build/compile_options.hpp` (`CMakeLists.txt:122-127`).
- **cmake/ modules:** `CompilerWarnings` (the strict warning set the tests mirror), `Sanitizers`, `Linker`, `EnableCcache`, `StaticAnalyzers`, `StandardProjectSettings`, and `CPM.cmake`.
- **Invocation:** `./configure.sh -t=Debug -p=build --use_clang` (writes `build.sh`) then `./build.sh`; CI mirrors this in `archlinux:base-devel` (clang + mold + ninja).

## Key Design Decisions
- **Qt6 Widgets (not QML):** the UI is a fixed set of dialogs, a data tree, and imperative command flows — Widgets + `.ui` files give declarative layout with minimal boilerplate and a mature, single-file-per-window model; the heavy logic is deliberately kept out of the UI layer. QML's view/VM split adds indirection the app doesn't need.
- **Rust for config options (Corrosion + cxx):** TOML (de)serialization and `serde` defaults are more ergonomic and memory-safe in Rust; the `cxx` bridge gives a clean `::km::Config` ↔ C++ `ConfigOptions` boundary with the crate built as a staticlib straight into the CMake target — one extra language, but no new runtime dependency and the TOML round-trip is unit-tested in-crate (`config-option-lib/src/lib.rs:105`).
- **polkit/pkexec (not sudo):** fine-grained `auth_admin` authorization per action (a named polkit policy, not a blanket root), a visible terminal window for the user, and — via the `terminal-helper` sentinel — the **real** command exit code (D-Bus terminal launchers otherwise return before the command finishes). No password re-entry within a session.
- **CPM (not system packages) for fmt/frozen/Corrosion:** pinned, reproducible dependency versions vendored into the build tree, so a fresh `archlinux:base-devel` container builds identically; the only *system* deps are the ones the app genuinely needs (Qt6, libalpm, glib, PolkitQt6).
- **Shell-based test harnesses (not CTest):** each `run_*.sh` compiles the **real** sources with the project's full GCC warning set and asserts against **injected probes** (pure modules) or an offscreen Qt platform (UI) — zero CTest infra, works on a bare checkout, and the bash-only harnesses (k16/k17) exercise the shell helpers with sandboxed PATH stubs while the md5 gates prove the harnesses are read-only.

## Testing
17 standalone harnesses (no CTest). Run all: `for t in tests/run_*.sh; do bash "$t"; done`
- `run_chunk1.sh` — ConfWindow/utils/config-options path logic (real sources + moc, offscreen).
- `run_chunk2.sh` — `known_kernels` table/lookup assertions (pure).
- `run_chunk2_ui.sh` — Configure source-dropdown auto-populate (offscreen UI driver).
- `run_k5.sh` — bootloader detection (fake probes).
- `run_k6.sh` — per-bootloader boot instructions.
- `run_k7.sh` — `alpm_utils` package/repo availability (live read-only DB).
- `run_k8.sh` — `install_kernel` plan/execute/boot-instructions (injected runner).
- `run_k11.sh` — `Kernel::get_kernels` curated info-row + local-only passes (live DB).
- `run_k12.sh` — offscreen render of the tree cells, pseudo-row, and the 4×4 telemetry header.
- `run_k13.sh` — repo-add: C++ guard contract + `repo_add.sh` script paths (no-root).
- `run_k14.sh` — directory install (`install_from_directory`, injected runner).
- `run_k15.sh` — build-dir persistence (QSettings in a sandboxed `XDG_CONFIG_HOME`).
- `run_k16.sh` — `build_helper.sh` GPG three-needle extraction (bash-only, stubbed makepkg/gpg).
- `run_k17.sh` — `terminal-helper` sentinel launch/rc protocol (bash-only, fake terminals).
- `run_k18.sh` — distro-family detection (fake `os-release`).
- `run_k19.sh` — driver-gate decision table (injected `GateProbe`).
- `run_k20.sh` — kernel-info 15-field extraction (injected `KernelInfoProbe`).

**Quality gates:** the C++ build is **0-warning** (strict set from `cmake/CompilerWarnings.cmake`, mirrored in the harnesses); CI runs `clang-tidy -p build/Debug src/*.cpp` (`checks.yml`) and a clang-format 16 check; the k12 harness renders offscreen (`QT_QPA_PLATFORM=offscreen`) as the UI smoke; the bash/system harnesses prove read-only behavior via before/after md5 of `/etc/pacman.conf` and the local DB.
