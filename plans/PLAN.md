# PLAN — kernel-manager-features (5 features + runtime crash fix)

- **Cycle:** kernel-manager-features
- **Date:** 2026-08-30
- **Base branch:** `main` @ `4e26423` (prior cycle `kernel-manager-decoupling` complete, 8/8 chunks merged)
- **Goal:** Fix the custom-build runtime crash (`.testscript` ENOENT + "Failed to insert new source array"), and deliver 5 user features: data-driven kernel→source auto-populate, kernel-source dropdown with custom URL, kernel description tooltips, app icon branding, "Cancel"→"Close" rename.
- **Quality bar:** public-facing Arch tool aiming at millions of users. Idiomatic C++23/Qt6, clean and structured, unit tests where infra allows, zero regressions, no new compiler warnings.

## Context & State

(From Work/DEV_LOG.md, Work/MASTER_LOG.md, Work/Docs/PLAN.md — all re-verified against source on 2026-08-30; line refs below are current.)

- main @ `4e26423`, tree clean, no active workers, no stray `branch/chunk-*`. Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)`; CPM fmt 12.2.0 + frozen (pinned rev); Corrosion v0.6.1 cxx bridge (config-option-lib, 2 cargo tests). No CMake/ctest target (no-op). Smoke: `QT_QPA_PLATFORM=offscreen timeout 12 ./build/kernel-manager` → expect exit 124 (alive).
- **The crash (verified root cause):**
  1. `prepare_git_repo` (utils.cpp:185-237) leaves the process **CWD inside the cloned repo** — final `enter(repo_path)` at :228-230 is never undone (the only restore, :210, lives in the stale-origin branch).
  2. `discover_repo_flavors` (conf-window.cpp:138-190, path set at :185) yields **bare subdir names**; for a single-PKGBUILD repo (e.g. AUR `linux-cachyos`) the fallback (:148-150) yields the repo **basename** `"linux-cachyos"`.
  3. `get_source_array_from_pkgbuild` (conf-window.cpp:336-342) builds `{path}/.testscript` → relative against repo-root CWD → `<repo>/linux-cachyos/.testscript` → `write_to_file` ENOENT (the observed `[WRITE_TO_FILE] 'linux-cachyos/.testscript' open failed`). Same latent defect in `get_package_names_glob_from_pkgbuild` (:395-414, path at :400), `set_custom_name_in_pkgbuild` (:444-460, :445), and `on_execute`'s `build_working_path = saved_cwd + "/" + path` (:943-944; consumed by makepkg :951 and `finished_proc` via `m_build_conf_path` :511/:521).
  4. `insert_new_source_array_into_pkgbuild` (:416-442): `read_whole_file` returns "" when the PKGBUILD is missing/unreadable, but the function still calls `write_to_file` (ENOENT dir / empty file) → false → `m_running=false` + "Failed to insert new source array into pkgbuild". No missing-PKGBUILD guard.
  - Secondary hardening: `run_and_remove_testscript` (:318-334) execs even when the script write fails (popen → "-1" noise); `get_pkgext_value_from_makepkgconf` (:344-357) drops `.testscriptpkgext` in the *process CWD* (repo root today; the app CWD after the fix) → pin it to the absolute repo path.
- **Current UI state (verified):** source field = QLineEdit `build_source_edit` (conf-options-page.ui:51; label :31-35 "Kernel source (AUR package name or git URL)"); populated in ctor from `utils::build_source_repo()` (conf-window.cpp:693, default `"linux-cachyos"` at utils.cpp:247); textChanged → `update_option_set()` (:694-696, :647-653); `sync_build_source()` (:640-643). Tree PkgName = `kernel.get_raw()` (km-window.cpp:93) — **repo-prefixed**: sync DB `"core/linux"` (kernel.cpp:293), AUR `"aur/linux-zen"` (kernel.cpp:344); `m_change_list` is keyed on the same raw name (km-window.cpp:50,62,320). No tooltip mechanism anywhere (zero `setToolTip`). No app-level `setWindowIcon` (only `.ui` theme iconsets: km-window.ui:19-21, conf-window.ui:19-22). Cancel buttons: km-window.ui:130-134 (`cancel`) and conf-options-page.ui:630-634 (`cancel_button`), both text "Cancel", both → `on_cancel()` → `close()` (km-window.cpp:372-374, conf-window.cpp:897-899). `treeKernels` context-menu policy set (km-window.cpp:229) with no handler (dead hook — out of scope this cycle).
- **Portal note (non-blocking, cosmetic):** running from the build dir prints "Could not register app ID: App info not found" (no installed .desktop). Resolves itself on installed use; do not gate this cycle on it.

## Known Kernels Data Model

New `src/known_kernels.hpp` (+ `src/known_kernels.cpp`) — single source of truth shared by chunk 2 (dropdown + auto-populate) and chunk 3 (tooltips).

```cpp
struct KnownKernel {
    std::string name;            // pkg name, repo prefix stripped, e.g. "linux"
    std::string display_name;    // human label, e.g. "Linux (mainline)"
    std::string description;     // 1-2 sentences: what it is, who it's for (tooltip)
    std::string default_source;  // build-source identifier (AUR name or git URL)
};
```
API (pure, unit-testable): `const std::vector<KnownKernel>& known_kernels();` · `std::optional<const KnownKernel*> find_kernel(std::string_view name);` · `std::string default_source_for(std::string_view name)` (fallback: the name itself) · `std::string description_for(std::string_view name, std::string_view category)` (fallback: synthesized from `Kernel::category()`, kernel.hpp:37-92) · `std::vector<std::string> known_sources()` (deduped, sorted `default_source` values).
Lookup key = package name with repo prefix stripped from the tree PkgName ("core/linux"→"linux", "aur/linux-zen"→"linux-zen").

Initial entries (name → default_source):

| name | display_name | default_source |
|---|---|---|
| linux | Linux (mainline) | **linux-cachyos** (user-specified override) |
| linux-zen | Linux Zen | linux-zen |
| linux-lts | Linux LTS | linux-lts |
| linux-cachyos | Linux CachyOS | linux-cachyos |
| linux-hardened | Linux Hardened | linux-hardened |
| linux-rt | Linux RT | linux-rt |

Every entry carries a 1-2 sentence `description` (e.g. linux: "Arch's default mainline kernel, tracking upstream releases closely — the general-purpose choice for desktops and servers."; linux-zen: "Tuned for desktop interactivity and gaming: zen scheduling, performance defaults."; linux-lts: "Long-term-support kernel with a slower update cadence, for stability-critical workloads."; linux-cachyos: "CachyOS kernel: performance-tuned mainline with LTO, EEVDF and BBR3 on by default."; linux-hardened: "Security-hardened kernel (KASLR, SMEP/SMAP, fortified builds) for threat-aware users."; linux-rt: "PREEMPT_RT real-time patched kernel for low-latency audio, video, and embedded workloads.").
Fallback rule: unknown name → `default_source = name`; description synthesized as `"<display_name> — <category> kernel (no curated description yet)"`.
The `linux`→`linux-cachyos` mapping is a user-specified override expressed as *data* (one table row), not control flow — future adjustments = edit one row.

## Chunks

### CHUNK 1 — Fix the custom-build runtime crash `[CRITICAL-PATH]`
Scope:
- `utils.cpp` `prepare_git_repo` (:185-237): save the entry CWD; restore it on **every** exit path (success and failure) before return.
- `conf-window.cpp` `discover_repo_flavors` (:138-190): make `KernelFlavor::path` **absolute** — subdir flavors `utils::build_repo_path() / dir`; single-PKGBUILD fallback = `utils::build_repo_path()` (repo root). Update the `KernelFlavor` doc comment (conf-window.hpp:50-56).
- `conf-window.cpp` `on_execute` (:943-944): `cpusched_path` is now absolute → `build_working_path = cpusched_path` (drop the `saved_cwd + "/"` concatenation); `m_build_conf_path` receives the absolute path (used by `finished_proc` :511, :521).
- `conf-window.cpp` `insert_new_source_array_into_pkgbuild` (:416-442): guard — missing/unreadable PKGBUILD (empty `read_whole_file` result) ⇒ do NOT write, return false with a clear stderr message.
- Hardening: `run_and_remove_testscript` (:318-334) returns early on script-write failure; `get_pkgext_value_from_makepkgconf` (:344-357) writes `.testscriptpkgext` to `utils::build_repo_path()` instead of the process CWD.
Files: `src/utils.cpp`, `src/conf-window.cpp`, `src/conf-window.hpp` (comment only).
Acceptance:
1. Build + offscreen smoke: no `[WRITE_TO_FILE] '…/.testscript' open failed` / "Failed to insert new source array into pkgbuild" on the linux-cachyos build path; app stays alive (exit 124).
2. Process CWD after `prepare_build_environment` equals the CWD before the call.
3. `kernel_path_for_index(i)` returns an absolute path that exists on disk once the repo is prepared.
Test strategy: build + offscreen smoke; standalone g++ unit test (chunk-4 precedent — no CTest infra) on the pure path logic: absolute form of `discover_repo_flavors` output (single-PKGBUILD dir → repo root; multi-subdir dir → root/dir) and the CWD save/restore helper.

### CHUNK 2 — Source dropdown + data-driven auto-populate `[COUPLED-TO: 1]`
Scope:
- New `src/known_kernels.{hpp,cpp}` per the data model above; add to `qt_add_executable` (CMakeLists.txt:128-143).
- `conf-options-page.ui`: replace QLineEdit `build_source_edit` (:51) with a QComboBox `build_source_combo` + a hidden QLineEdit `build_source_custom_edit` (keep the row `build_source_widget` :28-54; update label :31-35 to "Kernel source (known package or custom git URL)").
- `conf-window.cpp` ctor (:691-697): populate the combo from `known_sources()` + a "Custom URL…" entry; selecting "Custom URL…" shows + focuses the custom line edit; selecting a known item hides it. `sync_build_source()` (:640-643) and `update_option_set()` (:647-653) key off the **effective source** (custom text when the custom row is selected, else the combo item) — the `normalize_repo_key`/`resolve_option_map` machinery is unchanged.
- Auto-populate: new public `void ConfWindow::apply_source_for_kernel(std::string_view kernel_name)` — strips the repo prefix, maps via `default_source_for`, selects the matching combo item (or "Custom URL…" + prefilled line edit when the source is not in the known list). `MainWindow::on_configure` (km-window.cpp:355-370) calls it with the current tree PkgName **before** `sync_build_source()` (:361), so the background prepare (QtConcurrent, :365-369) clones the mapped source.
Files: `src/known_kernels.{hpp,cpp}` (new), `src/conf-options-page.ui`, `src/conf-window.{cpp,hpp}`, `src/km-window.cpp` (on_configure only), `CMakeLists.txt`.
Acceptance:
1. Dropdown shows the known sources (linux-cachyos, linux-zen, linux-lts, linux-hardened, linux-rt) + "Custom URL…"; known selection hides the line edit and updates the option rows; a custom URL round-trips through `sync_build_source` → `prepare_build_environment`.
2. Select `core/linux` in the tree → Configure → combo auto-selects `linux-cachyos` (the user-specified mapping, data-driven).
3. Select `aur/linux-zen` → auto-selects `linux-zen`; an unknown kernel (e.g. `linux-rc`) → "Custom URL…" prefilled with the kernel name.
4. `update_option_set` still toggles the 17 option rows per source (no regression).
Test strategy: standalone unit test on the pure table functions (`default_source_for` incl. the linux→linux-cachyos row, `find_kernel`, `known_sources` dedupe/sort, fallback-to-name); build + offscreen smoke.

### CHUNK 3 — Kernel description tooltips `[COUPLED-TO: 2]`
Scope: `init_kernels_tree_widget` (km-window.cpp:89-106) — after `setText(TreeCol::PkgName, …)` (:93), call `widget_item->setToolTip(TreeCol::PkgName, …)` with `description_for(prefix-stripped name, kernel.category())`. Both call sites are covered since the ctor init and `init_kernels` (km-window.cpp:380-394) share the helper.
Files: `src/km-window.cpp`; reuses `src/known_kernels.{hpp,cpp}` (no new declarations expected in km-window.hpp).
Acceptance: hovering PkgName shows a 1-2 sentence description (`linux` → mainline explanation); kernels not in the table show the synthesized category line; no other behavior changes.
Test strategy: unit test on `description_for` (known entry + synthesized fallback); offscreen smoke; live-session hover verified at Phase 4 QA.

### CHUNK 4 — App icon branding `[ISOLATED]`
Scope:
- New `src/km_icons.qrc` embedding shipped hicolor PNGs — `icons/48x48/org.archlinux.KernelManager.png` + `icons/256x256/org.archlinux.KernelManager.png` under prefix `/km-icons` (single alias `org.archlinux.KernelManager.png`).
- `CMakeLists.txt`: add the qrc to the executable sources (AUTORCC compiles it; unlike km_locale.qrc there is no build-dir copy step — icons live in the source tree).
- `main.cpp`: after `QApplication app(argc, argv)` (:130) call `app.setWindowIcon(QIcon(":/km-icons/org.archlinux.KernelManager.png"))`, with a one-line stderr notice if the resulting icon is null (smoke-testable). Keep the `.ui` theme iconsets (km-window.ui:19-21, conf-window.ui:19-22) — they serve installed use; the app-level icon covers running from the build dir. Packaging (10 hicolor sizes, :218-257; .desktop `Icon=`) unchanged.
Files: `src/km_icons.qrc` (new), `CMakeLists.txt`, `src/main.cpp`.
Acceptance:
1. The qrc compiles (rcc output present in build) and the embedded `QIcon` is non-null (verified via main's stderr line or a standalone check).
2. `./build/kernel-manager` shows the icon in taskbar + window titlebar (live session); offscreen smoke exit 124.
3. Installed package still ships the 10 hicolor sizes + desktop (unchanged).
Test strategy: build + resource/icon-null check + offscreen smoke; taskbar visibility confirmed manually at Phase 4 QA.

### CHUNK 5 — Rename "Cancel" to "Close" `[ISOLATED]`
Scope: button text only — `km-window.ui:130-134` (`cancel`) and `conf-options-page.ui:630-634` (`cancel_button`): "Cancel" → "Close". Keep object names, connections, and `on_cancel` handlers (they already `close()`). Do NOT touch the QProgressDialog built-in cancel (km-window.cpp:216-220). Run `lupdate` resync over `lang/*.ts` for the changed .ui source strings (16 locales, de-branded precedent from prior cycle).
Files: `src/km-window.ui`, `src/conf-options-page.ui`, `lang/*.ts` (lupdate resync).
Acceptance: both buttons read "Close"; clicking still closes the respective window (wiring unchanged); lupdate resync clean (no orphaned strings beyond the known pre-existing uk qrc-alias gap).
Test strategy: build + offscreen smoke; string verification (grep/strings) at QA.

## Dependency & Schedule

Chain: 1 → 2 → 3 coupled (2 builds on 1's absolute paths; 3 reuses 2's table and touches km-window.cpp). 4 isolated, parallel with 1. 5 isolated, scheduled after 3 so its `conf-options-page.ui` edit lands after 2's combo conversion is merged (one late .ui pass, no rebase friction).

Sliding window (Impl / Review agents; each on its own `branch/chunk-N` worktree off `main`):

| Iter | Lanes (parallel) |
|---|---|
| 1 | Impl 1 ‖ Impl 4 |
| 2 | Review 1 ‖ Review 4 ‖ Impl 2 (starts after 1 merges) |
| 3 | Review 2 ‖ Impl 3 (starts after 2 merges) |
| 4 | Review 3 ‖ Impl 5 (starts after 3 merges) |
| 5 | Review 5 → merge |
| — | Phase 4: full QA on main (build, offscreen smoke, cargo test 2/2, manual: taskbar icon + hover tooltip + Close buttons + dropdown/auto-populate, lupdate audit, packaging install-tree spot check) |
| — | Phase 5: SysOps compaction (DEV_LOG → MASTER_LOG; Changelog.md user-visible entry) |

## Risk & Mitigation

- **File overlap:** `conf-window.cpp` (1,2) and `km-window.cpp` (2,3) each have two chunks → sequenced 1→2→3 in the same lane, never parallel writers. `CMakeLists.txt` (2,4 — adjacent source-list lines) → 4 merges in Iter 2 before 2 branches; trivial rebase if lines collide. `conf-options-page.ui` (2,5) → 5 only after 2 is merged (Iter 4). Lease board (`@@@ ACTIVE_WORKERS @@@`) enforced per file set.
- **Mapping semantics:** `core/linux`→`linux-cachyos` is a user-specified override but lives in data (one table row), not control flow — future change = one-line edit; unknown kernels fall back to their own name, so no kernel becomes unbuildable.
- **CWD side effects:** restoring the CWD in `prepare_git_repo` changes where the pkgext testscript lands → explicitly pinned to the absolute repo path in chunk 1 (not process-CWD-relative); the async makepkg terminal already sets its own working directory (run_cmd_async :497), so build semantics are unaffected.
- **Icon/theme:** `.ui` theme iconsets retained — installed apps resolve hicolor icons exactly as before; the embedded app-level icon is the build-dir fallback. No packaging delta.
- **Portal app-ID error** (running from build dir): cosmetic, non-blocking — resolves on installed use; noted, not gated.
- **No CTest infra:** "unit tests" follow the established precedent (standalone g++ harness for pure logic; `cargo test` for the Rust bridge). No CMake test target added this cycle (scope guard).

## Definition of Done

1. Crash fixed: the linux-cachyos build path runs without `.testscript` ENOENT or source-array insertion failures; process CWD restored after prepare; offscreen smoke alive (exit 124).
2. All 5 features delivered and verified: dropdown + Custom URL entry; auto-populate (core/linux → linux-cachyos, data-driven); PkgName tooltips; taskbar/window icon; "Close" buttons.
3. Clean Release build (no new warnings beyond pre-existing `utils.cpp:101`), `cargo test` 2/2 in config-option-lib, table-logic unit tests pass.
4. No regressions: option-row toggling, patches tab, flavor combo, polkit escalation paths, and packaging install tree (desktop + 10 hicolor icons) behave as before.
5. lupdate resync complete for changed .ui strings; Changelog.md updated; cycle compacted to MASTER_LOG.
