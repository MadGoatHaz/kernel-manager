# PLAN — kernel-manager-kernels (21-kernel table + pre-compiled install + boot guidance)

- **Cycle:** kernel-manager-kernels
- **Date:** 2026-08-30
- **Base branch:** `main` @ `33fe35a` (cycle `kernel-manager-features` complete: 6-entry known-kernel table, source auto-populate, tooltips, app icon, Close buttons)
- **Goal:** Expand the curated kernel table from 6 to **21 mainstream maintained kernels**, each mapped to **its own** build source (remove the `linux`→`linux-cachyos` special case); give the app the ability to **install a pre-compiled kernel** (when one exists in a pacman repo) in addition to **building a custom one** (existing Configure flow); and after install, **instruct the user how to select the kernel at next boot** via **intelligent bootloader detection** (GRUB / systemd-boot / UKI / unknown).
- **Quality bar:** public-facing tool (millions of users). Idiomatic C++23/Qt6, data-driven (no control-flow special cases), unit tests per chunk, zero regressions, no new compiler warnings.
- **PROCESS RULE (binding):** every delegated task is SMALL and single-objective — **one clear goal, 1–3 files, its own tests, its own branch `branch/k<N>`, reviewed and merged before dependents move on**. If a chunk needs more than that, it must be split. Subagents never implement two chunks.

## Context & Corrections

1. **User correction (removes prior behavior):** the `linux`→`linux-cachyos` row in the known-kernel table (added in the features cycle as a "user-specified override") was a misinterpretation. Auto-populate must map **each kernel to its own package**: `core/linux`→`"linux"`, `linux-zen`→`"linux-zen"`, etc. The special case is removed in K1; the `linux` entry's description no longer mentions linux-cachyos.
2. **Scope expansion:** 21 mainstream maintained kernels (source of truth: `Work/Docs/Arch Linux Kernel Variants Audit.md`, 689 lines — repo/AUR availability, packaging clone URLs, preset/vmlinuz paths, UKI section; it has **no GRUB walkthrough**, so K6's GRUB text comes from standard ArchWiki knowledge, flagged in review).
3. **New capabilities:** (a) install pre-compiled package per kernel (repo-driven), (b) build custom (existing `on_configure` → ConfWindow flow, now fed by the corrected mapping), (c) post-install boot-selection instructions driven by detected bootloader. **No bootloader/boot code exists anywhere in the codebase today** — `src/bootloader.*` and `src/boot_instructions.*` are net-new modules.
4. **Current state (verified on main@33fe35a):**
   - `src/known_kernels.{hpp,cpp}`: `struct KnownKernel {name, display_name, description, default_source}` (6 entries; `default_source` = build-source identifier, AUR name or git URL); helpers `find_kernel`, `default_source_for`, `description_for`, `known_sources`, `kernel_name_from_raw` (all prefix-tolerant). Sole external consumer of `default_source_for`: `conf-window.cpp:742` (`ConfWindow::apply_source_for_kernel`); `known_sources()` feeds the dropdown at `conf-window.cpp:789`.
   - `src/km-window.{hpp,cpp,ui}`: `treeKernels` columns `TreeCol {Check, PkgName, Version, Category, Displayed, Immutable}` (Displayed/Immutable = hidden indices 4/5); context-menu policy `Qt::CustomContextMenu` set at km-window.cpp:235 with **no connected handler** (dead hook); "Choose" checkbox → `item_changed` → `build_change_list` → `on_execute` → worker thread → `Kernel::commit_transaction`.
   - `src/kernel.cpp`: `commit_transaction` runs `pacman -S --needed …` / `pacman -Rsn …` via `utils::runCmdTerminal(cmd, true)` (pkexec → `KM_HELPER_DIR/rootshell.sh`); AUR installs go through `detail::install_aur_kernels` (aur_kernel.cpp: `makepkg -sicf --cleanbuild`, non-escalated) when `ENABLE_AUR_KERNELS`.
   - `src/utils.cpp`: `exec()` (popen, returns "-1" on failure), `runCmdTerminal(cmd, escalate)` (terminal-helper; escalate wraps pkexec), `build_source_clone_url()` (bare name → `https://aur.archlinux.org/<name>.git`; `://` → used as-is), `prepare_build_environment()` (clones into `~/.cache/kernel-manager/pkgbuilds`).
   - `src/alpm_utils.{hpp,cpp}`: only `parse_alpm` (reads `/etc/pacman.conf`, registers every non-ignored section) + `release_alpm`.
   - Tests are **standalone** (no CTest): `tests/run_chunk*.sh` compile real sources with g++ C++23 + project warning set and run assertions; offscreen smoke `QT_QPA_PLATFORM=offscreen timeout 12 ./build/kernel-manager` → expect exit 124 alive, empty stderr.
   - Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)` → `./build/kernel-manager` (GCC 16.2.1, Qt6, libalpm 16.0.1). Baseline: 1 pre-existing warning (utils.cpp:103, `-Wignored-attributes`).

## Known Kernels Data — all 21 entries (per the audit report)

`install_repo` = first *pacman* repo listed in the report's matrix (core/extra/cachyos/liquorix/chaotic-aur); a leading "AUR"/"Source" entry alone ⇒ `install_repo="aur"`, `precompiled_available=false` (source-only; the custom build is the install path). `install_package` = the pre-compiled package name ("" when none). `build_source` = **the kernel's own package name** (identity) for all but `linux-tkg`, which is a build *framework* → its git URL (report: `https://github.com/Frogging-Family/linux-tkg.git`; produces `linux-tkg-<sched>` flavors, so it has no single pre-compiled package: `install_package=""`, `precompiled_available=false`).

| name | display_name | build_source | install_package | install_repo | pre | build |
|---|---|---|---|---|---|---|
| linux | Linux (mainline) | linux | linux | core | ✓ | ✓ |
| linux-lts | Linux LTS | linux-lts | linux-lts | core | ✓ | ✓ |
| linux-zen | Linux Zen | linux-zen | linux-zen | extra | ✓ | ✓ |
| linux-hardened | Linux Hardened | linux-hardened | linux-hardened | extra | ✓ | ✓ |
| linux-rt | Linux RT | linux-rt | linux-rt | extra | ✓ | ✓ |
| linux-rt-lts | Linux RT LTS | linux-rt-lts | linux-rt-lts | extra | ✓ | ✓ |
| linux-mainline | Linux Mainline | linux-mainline | linux-mainline | chaotic-aur | ✓ | ✓ |
| linux-cachyos | Linux CachyOS | linux-cachyos | linux-cachyos | cachyos | ✓ | ✓ |
| linux-cachyos-bore | Linux CachyOS BORE | linux-cachyos-bore | linux-cachyos-bore | cachyos | ✓ | ✓ |
| linux-cachyos-rt-bore | Linux CachyOS RT BORE | linux-cachyos-rt-bore | linux-cachyos-rt-bore | cachyos | ✓ | ✓ |
| linux-cachyos-lts | Linux CachyOS LTS | linux-cachyos-lts | linux-cachyos-lts | cachyos | ✓ | ✓ |
| linux-cachyos-server | Linux CachyOS Server | linux-cachyos-server | linux-cachyos-server | cachyos | ✓ | ✓ |
| linux-cachyos-deckify | Linux CachyOS Deckify | linux-cachyos-deckify | linux-cachyos-deckify | cachyos | ✓ | ✓ |
| linux-cachyos-bmq | Linux CachyOS BMQ | linux-cachyos-bmq | linux-cachyos-bmq | cachyos | ✓ | ✓ |
| linux-tkg | Linux TKG | https://github.com/Frogging-Family/linux-tkg.git | (none) | chaotic-aur | — | ✓ |
| linux-xanmod | Linux XanMod | linux-xanmod | linux-xanmod | chaotic-aur | ✓ | ✓ |
| linux-xanmod-edge | Linux XanMod Edge | linux-xanmod-edge | linux-xanmod-edge | chaotic-aur | ✓ | ✓ |
| linux-xanmod-lts | Linux XanMod LTS | linux-xanmod-lts | linux-xanmod-lts | chaotic-aur | ✓ | ✓ |
| linux-xanmod-rt | Linux XanMod RT | linux-xanmod-rt | linux-xanmod-rt | chaotic-aur | ✓ | ✓ |
| linux-lqx | Linux Liquorix | linux-lqx | linux-lqx | liquorix | ✓ | ✓ |
| linux-clear | Linux Clear | linux-clear | linux-clear | chaotic-aur | ✓ | ✓ |

`description` (1–2 sentences each, from the report): **linux** "Arch's default kernel — upstream stable releases; the general-purpose reference for desktops and servers." · **linux-lts** "Long-Term-Support kernel on the designated stable LTS branch — maximum stability and hardware compatibility." · **linux-zen** "Low-latency desktop kernel with the collaborative Zen patchset (tuned schedulers, memory, high-frequency timers) — a good daily driver." · **linux-hardened** "Security-hardened kernel with exploit-mitigation patches (dmesg restriction, strict RWX, page poisoning) for threat-aware users." · **linux-rt** "PREEMPT_RT real-time kernel (Molnar/Gleixner patchset) — deterministic, bounded latency for audio, automation and control workloads." · **linux-rt-lts** "PREEMPT_RT on an LTS base — real-time determinism with long-term-support stability." · **linux-mainline** "Tracks Linus' master branch and weekly release candidates — newest hardware enablement, pre-stable." · **linux-cachyos** "CachyOS's heavily optimized mainline kernel — Clang ThinLTO, AutoFDO, Propeller, BORE scheduler; maximum desktop/gaming performance." · **linux-cachyos-bore** "Dedicated BORE (Burst-Oriented Response Enhancer) scheduler variant for interactive latency and frame pacing." · **linux-cachyos-rt-bore** "PREEMPT_RT + BORE — low-jitter real-time audio and simulation." · **linux-cachyos-lts** "LTS base with the CachyOS patchset — stable desktop with performance tuning." · **linux-cachyos-server** "Server-oriented kernel — lazy preemption and server EEVDF tuning for high-concurrency and virtualization nodes." · **linux-cachyos-deckify** "Optimized for handheld gaming consoles (Steam Deck, MSI Claw)." · **linux-cachyos-bmq** "BMQ (BitMap Queue) alternative-runqueue variant for scheduler benchmarking (no sched-ext)." · **linux-tkg** "Frogging-Family's modular custom-kernel framework — user-selected scheduler (BORE/BMQ/PDS/EEVDF), compiler and patches; built from the tkg repo." · **linux-xanmod** "Performance kernel — memory-allocation tuning, high-frequency ticks, BBRv3 and CAKE queueing; for multimedia and low latency." · **linux-xanmod-edge** "XanMod on a mainline (edge) base — experimental features with low-latency desktop tuning." · **linux-xanmod-lts** "XanMod on an LTS base — stable desktop with XanMod subsystems." · **linux-xanmod-rt** "XanMod + PREEMPT_RT — deterministic processing with XanMod enhancements." · **linux-lqx** "Liquorix — Zen-based kernel tuned for desktop audio latency, multimedia and low-latency preemption." · **linux-clear** "Intel Clear Linux performance and power-management patchset port — optimized for Intel platforms."

## Data Model Change (decided — one clean scheme)

**Rename** `default_source` → `build_source` and `default_source_for()` → `build_source_for()` (no duplication of the two fields); update the single call site `conf-window.cpp:742`. Keep `find_kernel`, `description_for`, `known_sources` (reads `build_source`), `kernel_name_from_raw` unchanged.

```cpp
struct KnownKernel {
    std::string name;            // pkg name, repo prefix stripped
    std::string display_name;    // human label
    std::string description;     // 1-2 sentences (tooltip)
    std::string build_source;    // its own AUR/pkg name, or a git URL (tkg)
    std::string install_package; // pre-compiled pkg name, "" if none
    std::string install_repo;    // core|extra|cachyos|chaotic-aur|aur|liquorix
    bool precompiled_available;  // a pre-compiled path is known (report data)
    bool buildable;              // a custom build is possible (all 21: true)
};
```

New helpers (prefix-tolerant like the existing ones): `std::string install_package_for(name)` (→ "" for unknown), `std::string install_repo_for(name)` (→ "" for unknown), `bool is_installable(name)` (entry && `precompiled_available` && `!install_package.empty()`; unknown → false). **Fallback for unknown kernels:** `build_source=name`, `install_package=name`, `install_repo=""`, `precompiled_available=false`, `buildable=true` (never unbuildable — preserves today's invariant).

## Bootloader Detection (net-new `src/bootloader.{hpp,cpp}`)

```cpp
enum class Bootloader { GRUB, SYSTEMD_BOOT, UKI, UNKNOWN };
std::string bootloader_name(Bootloader bl);                    // "GRUB" | "systemd-boot" | "UKI" | "unknown"
struct BootloaderProbe {                                        // injected for unit tests
    std::function<bool(std::string_view path)> path_exists;     // file or dir
    std::function<bool(std::string_view cmd)>  command_exists;  // `command -v`
    std::function<bool(std::string_view dir)>  dir_has_uki;     // any *.efi in dir
};
Bootloader detect_bootloader(const BootloaderProbe& probe);     // pure: only uses the probe
Bootloader detect_bootloader();                                 // real probe: std::filesystem + utils::exec("command -v …")
```

Heuristics, in priority order (first match wins):
1. **UKI** — `dir_has_uki("/boot/EFI/Linux")` or `dir_has_uki("/efi/EFI/Linux")`.
2. **SYSTEMD_BOOT** — `command_exists("bootctl")` AND (`path_exists("/boot/loader/loader.conf")` OR `path_exists("/efi/loader/loader.conf")` OR `path_exists("/boot/loader/entries")`).
3. **GRUB** — `path_exists("/boot/grub/grub.cfg")` OR `path_exists("/etc/grub.d")` OR `command_exists("grub-mkconfig")` OR `command_exists("grub-editenv")`.
4. **UNKNOWN** — else.

Testable by construction: unit tests call `detect_bootloader(fakeProbe)` only (no FS, no commands).

## Boot Instructions (net-new `src/boot_instructions.{hpp,cpp}`)

`std::vector<std::string> instructions_for(Bootloader bl, const std::string& kernel_pkgbase);` — ordered human steps, `P` = kernel pkgbase. **Always append the final note:** "The kernel binary is at `/boot/vmlinuz-<P>`; the initramfs is regenerated automatically on install (ALPM hook) or via `sudo mkinitcpio -P`."
- **GRUB:** ① "Re-run the GRUB config: `sudo grub-mkconfig -o /boot/grub/grub.cfg`" ② "Reboot and select '<P>' from the GRUB menu" ③ "To make it the default: set `GRUB_DEFAULT` in `/etc/default/grub`, then re-run `grub-mkconfig`".
- **SYSTEMD_BOOT:** ① "If a UKI is enabled for this kernel, its `.efi` in `/boot/EFI/Linux/` is auto-detected — just reboot and select it" ② "Otherwise create `/boot/loader/entries/<P>.conf` with: title, `linux /boot/vmlinuz-<P>`, `initrd /boot/initramfs-<P>.img`" ③ "Verify with `bootctl`" ④ "Reboot and select '<P>'" ⑤ "To set the default: `sudo bootctl set-default <P>`".
- **UKI:** ① "The Unified Kernel Image is auto-detected by the firmware/systemd-boot — reboot and select '<P>'".
- **UNKNOWN:** ① "Reboot and choose '<P>' from your boot menu" ② "If it does not appear, regenerate your bootloader config (GRUB: `sudo grub-mkconfig -o /boot/grub/grub.cfg`; systemd-boot: ensure a loader entry or UKI exists)".

Pure function of its inputs (no FS access) → trivially unit-testable.

## Chunks

**K1 [CRITICAL-PATH] — correct the mapping + extend the struct.** Goal: rename field/helper `default_source`→`build_source` / `default_source_for`→`build_source_for`, **remove the `linux`→`linux-cachyos` row** (linux: `build_source="linux"`, description without the cachyos sentence), add the 4 install fields + new helpers to the struct, keep all 6 existing entries valid. Files (3): `src/known_kernels.hpp`, `src/known_kernels.cpp`, `src/conf-window.cpp` (one-line rename at :742 + comment touch-ups). Acceptance: struct has `install_package/install_repo/precompiled_available/buildable`; `build_source_for("linux")=="linux"` and `build_source_for("core/linux")=="linux"` (NOT linux-cachyos); 6 entries intact; main build clean. Tests: extend the standalone pattern — new `tests/test_k1_mapping.cpp` + `tests/run_k1.sh` asserting the corrected mapping, new fields on the 6 entries, helper fallbacks. **Note:** `tests/test_chunk2_known_kernels.cpp` (still asserting the old mapping/API) is declared STALE by K1; it is superseded in-place by K4 (standalone script — does not block the main build or smoke in the interim).

**K2 [COUPLED-1] — official + CachyOS rows (data only).** Goal: make the table hold all **13** official + CachyOS rows per the data table above — 6 already exist after K1 (linux, linux-zen, linux-lts, linux-cachyos, linux-hardened, linux-rt), so **add 7 rows**: linux-rt-lts, linux-cachyos-bore/-rt-bore/-lts/-server/-deckify/-bmq. Files (1): `src/known_kernels.cpp`. Acceptance: table size == 13; every new row has correct `build_source` (identity), `install_package`/`install_repo`/flags per table; builds clean. Tests: `tests/test_k1_mapping.cpp` gains size==13 + spot-checks (run via `run_k1.sh`).

**K3 [COUPLED-2] — community rows (data only).** Goal: add the **8** community rows: linux-mainline (chaotic-aur pre-compiled, AUR build source), linux-tkg (github framework URL, no pre-compiled pkg), linux-xanmod/-edge/-lts/-rt (chaotic-aur), linux-lqx (liquorix), linux-clear (chaotic-aur). Files (1): `src/known_kernels.cpp`. Acceptance: table size == 21; mainline `install_repo=chaotic-aur`, `precompiled_available=true`; tkg `precompiled_available=false`, `install_package=""`, `build_source`=github URL; lqx `install_repo=liquorix`; builds clean. Tests: size==21 + spot-checks in the same harness.

**K4 [COUPLED-3] — full-table unit test (supersedes chunk-2 test).** Goal: rewrite `tests/test_chunk2_known_kernels.cpp` + `tests/run_chunk2.sh` in place to assert the complete 21-entry table: all names present + unique; every `description` non-empty (1–2 sentences); install-field consistency (`precompiled_available ⇒ install_package` non-empty; `install_repo` ∈ allowed set); corrected mapping (`linux`→`linux`, `linux-zen`→`linux-zen`, unknown→itself, prefix-tolerant); `known_sources()` deduped/sorted incl. the tkg URL. Files (2): the test + its runner script. Acceptance: harness passes standalone. (No production code touched.)

**K5 [ISOLATED] — bootloader detection module.** Goal: implement `src/bootloader.{hpp,cpp}` exactly per the spec above (injected `BootloaderProbe`; real probe via std::filesystem + `utils::exec("command -v …")`). Files (3): `src/bootloader.hpp`, `src/bootloader.cpp`, `CMakeLists.txt` (add the pair to `qt_add_executable`). Acceptance: `detect_bootloader()` returns a valid enum on this machine; `bootloader_name()` non-empty for all 4; module compiles with the project warning set; CMake line appended right after the `known_kernels` line. Tests: `tests/test_k5_bootloader.cpp` + `run_k5.sh` — fake probes: (uki dir with .efi→UKI) (bootctl+loader.conf→SYSTEMD_BOOT) (grub.cfg→GRUB) (empty→UNKNOWN) (UKI wins over GRUB when both present).

**K6 [COUPLED-5] — boot-instructions module.** Goal: implement `src/boot_instructions.{hpp,cpp}` per the spec (ordered steps incl. the mandatory vmlinuz/mkinitcpio note; GRUB text is standard ArchWiki knowledge — the audit report has no GRUB walkthrough, reviewer confirms wording). Files (3): `src/boot_instructions.hpp`, `src/boot_instructions.cpp`, `CMakeLists.txt` (append after K5's line). **Fork from `branch/k5`** (needs the `Bootloader` enum; rebase onto main after K5 merges). Acceptance: non-empty ordered steps for each bootloader; every result ends with the `/boot/vmlinuz-<P>` note; builds clean. Tests: `tests/test_k6_boot_instructions.cpp` + `run_k6.sh` — key strings per bootloader (grub-mkconfig line, bootctl set-default line, UKI auto-detect line, unknown fallback line, final note present 4/4).

**K7 [ISOLATED] — package-availability helper.** Goal: add to `src/alpm_utils.{hpp,cpp}`: `bool is_package_available(std::string_view pkg, std::string_view repo)` — pacman repos (core/extra/cachyos/chaotic-aur/liquorix): fresh `parse_alpm` + find sync DB by name + `alpm_db_get_pkg` (read-only, no root; missing repo section ⇒ false); `repo=="aur"`: `command -v paru` + `paru --aur -Si <pkg>` non-empty (graceful false without paru). Split for testability: `PackageSource classify_repo(repo)` (pure), `bool is_aur_package_available(exec_fn, pkg)` (injected `exec_fn`), `bool is_package_in_sync_db(alpm_handle_t*, repo, pkg)` (real). Files (2): `src/alpm_utils.hpp`, `src/alpm_utils.cpp`. Acceptance: correct true/false for a known core pkg (`linux`) and an AUR pkg on this machine; no root required; no new warnings. Tests: `tests/test_k7_package_availability.cpp` + `run_k7.sh` — `classify_repo` cases; `is_aur_package_available` with fake exec (present/absent/no-paru); `is_package_in_sync_db` smoke against a real `parse_alpm` handle (core/linux ⇒ true).

**K8 [COUPLED-1,6,7] — pre-compiled install action + post-install.** Goal: new `src/install_kernel.{hpp,cpp}`: `struct InstallStep {std::string cmd; bool escalate;}`; **pure** `std::vector<InstallStep> plan_steps(const KnownKernel&, Bootloader bl)` — pre-compiled available: `pacman -S --needed <install_package>` (escalated, **unless `install_repo=="aur"`** → `paru -S --needed <pkg>` non-escalated; no paru ⇒ fall back to the existing AUR makepkg path, mirroring aur_kernel.cpp) → always `mkinitcpio -P` (escalated) → bootloader refresh: GRUB only `grub-mkconfig -o /boot/grub/grub.cfg` (escalated); SYSTEMD_BOOT/UKI/UNKNOWN: none (auto); not pre-compiled but buildable: the makepkg sequence from the prepared repo. Then `struct InstallKernelResult {bool ok; std::string error; std::vector<std::string> boot_instructions;}`; `InstallKernelResult install_kernel(const KnownKernel&, CommandRunner runner = <utils::runCmdTerminal>, Bootloader bl = detect_bootloader())` — executes the plan, **graceful on any step failure** (ok=false + error text, never crashes), fills `boot_instructions = instructions_for(bl, kernel.name)`. Files (3): `src/install_kernel.hpp`, `src/install_kernel.cpp`, `CMakeLists.txt` (append after K6's line). **Fork from `branch/k6`** (has K6 + K5; base main already has K1+K7 after Iter 2; rebase onto main after K6 merges). Acceptance: plan chosen correctly by repo; step sequence correct per bootloader; failure ⇒ clean error + no crash; instructions returned. Tests: `tests/test_k8_install_kernel.cpp` + `run_k8.sh` — `plan_steps` cases (core linux + GRUB; AUR-chaotic pkg + systemd-boot; tkg/buildable-only; unknown fallback) and `install_kernel` with a recording `CommandRunner` (assert exact cmd/escalate sequence; runner returns 1 ⇒ ok=false, error non-empty, no abort).

**K9 [COUPLED-8,6] — main-window context menu + wiring.** Goal: connect the dead `customContextMenuRequested` on `treeKernels` (km-window.cpp:235) — right-click a row → QMenu with 3 actions: **"Install pre-compiled <kernel>"** (enabled iff `km::is_installable(name)`; runs K8 in a worker with the existing progress-dialog pattern, reports result + shows the boot instructions), **"Build custom <kernel>"** (selects the row, then the existing `on_configure()` flow), **"Show boot instructions"** (dialog: numbered steps from `instructions_for(detect_bootloader(), name)`, copyable text). Files (2–3): `src/km-window.hpp` (slot decls), `src/km-window.cpp` (connect + handlers); `src/km-window.ui` only if a static menu is preferred (code-built QMenu recommended → expect 2 files). **Fork from `branch/k8`** (rebase onto main after K8 merges). Acceptance: menu appears on right-click with correct enable states; Install triggers K8 end-to-end; Build triggers existing Configure; instructions dialog shows K6 output; builds clean; no regression to checkbox/Execute flow. Tests: offscreen smoke (app alive exit 124) + a small Qt driver (follow run_chunk2_ui.sh precedent) asserting the 3 actions exist for a curated row and Install is disabled for `linux-tkg`.

**K10 [COUPLED-9] — install-availability indicator column.** Goal: new visible "Install" column on `treeKernels` between Category and the hidden Displayed/Immutable columns — **renumber `TreeCol`** (`Check, PkgName, Version, Category, Install, Displayed, Immutable`) and add the header column to `km-window.ui` in the matching position; row value in `init_kernels_tree_widget`: ✓ (tooltip: repo name) when `km::is_installable(name)` AND (pacman repo ⇒ live `is_package_available` from the worker init; `aur` ⇒ static ✓ per table) else — (tooltip explains: "build only" for tkg / "repo not added" if the live check failed). Value is data + one local alpm query (no network) so it stays fast. Files (2): `src/km-window.hpp` (enum), `src/km-window.cpp` + `src/km-window.ui` (column) — if that exceeds 3 files, split ui from cpp into K10a/K10b (reviewer may require the split). **Fork from `branch/k9`** (rebase after K9 merges). Acceptance: every row shows a correct Install marker; existing columns/checks/Execute flow untouched; builds clean. Tests: offscreen smoke + Qt driver asserting the Install column value for `core/linux` (✓) and `linux-tkg` (—) plus correct column count/enum indices.

## Schedule (small chunks; disjoint file sets per iteration; fork-from-branch + rebase-on-merge)

| Iteration | Parallel lanes | Fork bases |
|---|---|---|
| 1 | Impl **K1** ‖ Impl **K5** ‖ Impl **K7** (known_kernels.* / bootloader.* / alpm_utils.* — disjoint) | all from `main@33fe35a` |
| 2 | Review **K1+K5+K7** ‖ Impl **K2** (after K1 merge) ‖ Impl **K6** (fork `branch/k5`) | K6: `branch/k5` → rebase on main after K5 merge |
| 3 | Review **K2+K6** ‖ Impl **K3** (after K2 merge) ‖ Impl **K8** (fork `branch/k6`; main has K1+K5+K7) | K8: `branch/k6` → rebase on main after K6 merge |
| 4 | Review **K3+K8** ‖ Impl **K4** (fork `branch/k3` — needs the 21 rows) ‖ Impl **K9** (fork `branch/k8`) | K4: `branch/k3` → rebase after K3 merge; K9: `branch/k8` → rebase after K8 merge |
| 5 | Review **K4+K9** ‖ Impl **K10** (fork `branch/k9`) | K10: `branch/k9` → rebase after K9 merge |
| 6 | Review **K10** → merge | — |
| Phase 4 | Full QA on merged `main`: clean Release build (warning set == baseline: only utils.cpp:103), **all** standalone harnesses green (k1,k4/k2,k5,k6,k7,k8 + chunk-1 + UI drivers), offscreen smoke exit 124 empty stderr, live checks (context menu, Install flow on a real kernel, boot-instructions dialog), no regressions | |
| Phase 5 | Log compaction: milestones → `Work/MASTER_LOG.md`, clear `DEV_LOG.md` finished entries | |

**File-conflict guard:** `known_kernels.cpp` = single lane K1→K2→K3→K4 (sequential only). `CMakeLists.txt` = one appended line per module, sequential chain K5→K6→K8 (each forks from the prior, so no concurrent edits). `km-window.*` = K9→K10 sequential. Reviewer for any chunk re-runs the full harness set it can compile.

## Risk & Mitigation

- **Stale test window (K1→K4):** `test_chunk2_known_kernels.cpp` fails after K1 (old mapping + renamed API). Mitigation: it is a standalone script (not in CMake — main build/smoke unaffected); K4 rewrites it in place in Iter 4, well before Phase 4 QA; K1's review records this explicitly.
- **Bootloader heuristics vary per distro/install** (mixed /boot vs /efi, bootctl present but no systemd-boot config). Mitigation: priority order is conservative (UKI > systemd-boot > GRUB > UNKNOWN); the module is probe-injected and unit-tested on all four outcomes; UNKNOWN instructions cover both regenerators; reviewer sanity-checks `detect_bootloader()` on this machine (expected: systemd-boot or GRUB) in Phase 4.
- **Third-party repos (cachyos/chaotic-aur/liquorix) may not be added on a user's system** ⇒ `is_package_available` false even though the table says pre-compiled exists. Mitigation: table flag = "a pre-compiled path is documented"; the live K7 check is the authority; K10's — tooltip says which repo to add; K8 fails gracefully with an actionable message (never installs from a repo that isn't there).
- **AUR pre-compiled installs need paru** (or a makepkg fallback). Mitigation: same pattern aur_kernel.cpp already uses; absence of paru ⇒ build path, not an error.
- **tkg's `install_package` is ""** — the "Install pre-compiled" menu item must be disabled for it (K9 enable rule uses `km::is_installable`).
- **Root escalation** for pacman/mkinitcpio/grub-mkconfig reuses the existing pkexec `rootshell.sh` path (`runCmdTerminal(cmd, true)`) — no new policy files.
- **GCC 16 strict warnings** on new modules: all new code is written under the project warning set from day one (chunk harnesses use it; CI build must show no new warnings).

## Definition of Done

All 21 kernels curated with **correct own-source mappings** (identity for all but tkg; `linux`→`linux`, **no** linux-cachyos special case); per kernel the app can **install the pre-compiled package** (repo-aware, graceful) **and build a custom one** (existing Configure flow); **bootloader is detected** and **boot instructions are shown** post-install (GRUB/systemd-boot/UKI/unknown); clean Release build with **no new warnings** (baseline utils.cpp:103 only); **all unit harnesses pass**; offscreen smoke alive (exit 124, empty stderr); context menu + Install column work without regressions to the existing checkbox/Execute flow.
