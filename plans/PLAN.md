# PLAN — kernel-manager-enhancements (config expansion + iconography + install truth)

- **Cycle:** kernel-manager-enhancements
- **Date:** 2026-08-31
- **Base branch:** `main` @ `4c66803` (prev. cycle `kernel-manager-kernels` archived at `b5c3e13`; tree 341/341 green, warning baseline = exactly 1 at `src/utils.cpp:103` `-Wignored-attributes`)
- **Objectives:** (1) expand the Configure module's per-repo build-option coverage beyond `linux_cachyos`; (2) fix application iconography for GUI windows AND the system taskbar; (3) make the main-window kernel list complete and its Install column truthful.
- **Quality bar:** public-facing tool. C++23/Qt6, data-driven (no control-flow special cases), one branch per chunk (`branch/e<N>`), unit tests per chunk, zero regressions, no new compiler warnings.
- **PROCESS RULE (binding):** atomic single-file micro-chunks (≤ ~50–100 changed lines). One primary file per chunk; a test file + its `run_*.sh` runner count as one chunk (K8 precedent). Each chunk: implement → verify (build + harnesses + smoke) → review → `--no-ff` merge → dependents move.

---

## Verified current state (recon evidence, 2026-08-31)

**Objective 1 — option machinery**
- `src/compile_options.json`: **only** `linux_cachyos`, a 17-entry map option-key → PKGBUILD build var (`hardly`→`_cc_harder`, `HZ_ticks`→`_HZ_ticks`, `tickrate`→`_tickrate`, `preempt`→`_preempt`, `hugepage`→`_hugepage`, `cpu_opt`→`_processor_opt`, `lto`→`_use_llvm_lto`, `localmodcfg`→`_localmodcfg`, `per_gov`→`_per_gov`, `tcp_bbr3`→`_tcp_bbr3`, `custom_config`→`_cachy_config`, `nconfig`→`_makenconfig`, `xconfig`→`_makexconfig`, `use_current`→`_use_current`, `builtin_zfs`→`_build_zfs`, `builtin_nvidia_open`→`_build_nvidia_open`, `build_debug`→`_build_debug`). All 17 names verified against the live `CachyOS/linux-cachyos@master/linux-cachyos/PKGBUILD` (fetched; defaults `${_var:=...}` match exactly).
- `src/mkoptions.py`: CMake custom command (CMakeLists.txt:115-121) regenerates `$BUILD/compile_options.hpp` on JSON/script mtime change; emits one `constexpr frozen::unordered_map<frozen::string, std::string_view, N> <json_key>` per JSON top-level key in `namespace detail`.
- `src/conf-window.cpp`: `resolve_option_map` (:288-293) hard-codes `if (repo_key == "linux_cachyos") return &detail::linux_cachyos; return std::nullopt;`. `option_build_var` (:297-305) nullopt-aware lookup. `update_option_set` (:669-675) shows/hides each of the 17 `option_row_bindings` (:231-249) iff the active map offers its var → **every non-`linux_cachyos` source hides all 17 rows** (the "reduced generic set" is in practice the empty set). `get_all_set_values` (:569-607) emits `VAR=value\n` only for offered options; values come from combo helpers (`hz_tick` "1000"…; `tickless_mode` full/idle/periodic; `preempt_mode` full/lazy/voluntary/none; `lto_mode` none/full/thin/thin-dist; `hugepage_mode` always/madvise; `cpu_opt_mode` manual/native/generic_v1..v4/zen4, "manual" emits nothing) — `conf-window.cpp:106-111`. Emission reaches the build two ways: `utils::restore_clean_environment` setenv()s them into the makepkg child env, and the same string is sourced into the PKGBUILD testscript.
- `ConfigOptions` (17 fields) ↔ Rust `km::Config` TOML twin (save/load profile) is **orthogonal** to the per-repo maps and untouched by this cycle.

**Per-repo build-var facts (fetched live + audit doc `Work/Docs/Arch Linux Kernel Variants Audit.md`):**
- **CachyOS family — 1 repo, 10 flavors** (`linux-cachyos`, `-bore`, `-rt-bore`, `-lts`, `-server`, `-deckify`, `-bmq`, `-eevdf`, `-hardened`, `-rc` subdirs in `CachyOS/linux-cachyos`): every flavor PKGBUILD uses the **identical** `_`-var convention (verified on bore/hardened/lts PKGBUILDs: same `_HZ_ticks`/`_preempt`/`_processor_opt`/`_use_llvm_lto` defaults). Source dropdown entries for the 6 AUR siblings (`linux-cachyos-bore` etc., from the 21-row table) normalize to keys `linux_cachyos_bore`… → **currently all fall to nullopt → zero option rows** even though their PKGBUILDs honor the full 17-var set.
- **XanMod ×4** (`linux-xanmod[-edge|-lts|-rt]`, AUR): env vars `_microarchitecture` (numeric menu 0=generic … 11=Zen, 14=Zen4, 15=Zen5, 99=native-autodetect; via `choose-gcc-optimization.sh`), `_localmodcfg` (**y/n** — differs from cachyos yes/no), `use_numa`, `use_tracers`, `_compress_modules`, `_compiler=clang`. Only `_microarchitecture` + `_localmodcfg` map onto the existing 17 UI options; values need translation (see E1).
- **Liquorix** (`linux-lqx`, AUR/liquorix repo): `_localmodcfg` = **non-empty ⇒ enabled** (`[ -n "$_localmodcfg" ]`), empty default — "yes"/"no" pass-through would misbehave; needs translation too.
- **Official Arch ×6** (linux, lts, zen, hardened, rt, rt-lts): audit: "Official PKGBUILDs do not define external environment switches" → no map.
- **linux-mainline, linux-clear**: no documented build flags → no map.
- **linux-tkg**: its own `customization.cfg`/`install.sh` framework — env-var injection into `makepkg` does not apply → no map (Build-custom still works; TKG is configured through its own framework; documented, not emulated).

**Objective 2 — iconography**
- All 10 `icons/*/org.archlinux.KernelManager.png` + root 390×390 `org.archlinux.KernelManager.png` are **byte-identical (md5-verified) to the upstream `CachyOS/kernel-manager@develop` icons** — a dedicated green artwork (vertical bar + three connected circles, `#0BAC66`/`#11E388` on transparent), **not** the Wayland "W". The real Wayland logo (`wayland.freedesktop.org/wayland.png`, 117×150, W glyph) and this system's Qogir `scalable/apps/wayland.svg` (rendered: the W in a circle) are clearly different.
- Wiring: `src/main.cpp:137-143` sets the app window icon from `:/km-icons/org.archlinux.KernelManager.png` (single alias, rcc last-wins ⇒ one 256×256 image). **BUT both .ui files** (`km-window.ui:19-21`, `conf-window.ui:19-22`) set `windowIcon` from `<iconset theme="org.archlinux.KernelManager"/>`; uic generates per-window `setWindowIcon(QIcon::fromTheme(...))` (verified in `build/kernel-manager_autogen/include/ui_km-window.h:50-51`, `ui_conf-window.h:39-46`) — a **null icon when the theme icon is absent**, overriding the app icon on both windows.
- On this machine the theme icon **is absent**: no `org.archlinux.KernelManager` file anywhere under `/usr/share/icons`, `/usr/local/share/icons`, `~/.local/share/icons` (all three searched), and **no installed .desktop entry** (`/usr/share/applications`, `~/.local/share/applications` empty of kernel entries); the app runs from `./build/kernel-manager` only. CMake (CMakeLists.txt:198-261) and the PKGBUILD `package()` install 10 hicolor icons + `.desktop` **correctly** — so a packaged install shows the dedicated icon; a build-dir run has nothing for the panel to resolve.
- **Why the W:** with no resolvable `org.archlinux.KernelManager` icon/desktop, Plasma 6 (this system: `plasmashell`+`kwin_wayland`, Qogir theme) falls back to the theme's `wayland` app icon = the W (Qogir ships it at `/usr/share/icons/Qogir/scalable/apps/wayland.svg` — evidence the panel references that icon name as a placeholder). The .ui null-override additionally kills the bundled window icon on X11 (empty `_NET_WM_ICON`) for uninstalled runs.
- `app.setDesktopFileName("org.archlinux.KernelManager")` (main.cpp:129) is correct and keeps the app-id ↔ .desktop ↔ hicolor names aligned.

**Objective 3 — list + Install column**
- List = `Kernel::get_kernels` (src/kernel.cpp:257-352): walks **registered sync DBs** (`/etc/pacman.conf` sections via `parse_alpm`) searching `linux[^ ]*-headers`, pairs each `<name>-headers` with `<name>` (skips if the kernel pkg itself is missing), plus an AUR pass via `paru --aur -Sl` **only under `#ifdef ENABLE_AUR_KERNELS` — which is NOT defined in CMakeLists (verified: no such option; only `KM_IGNORE_REPO` + `HAVE_ALPM_INSTALLED_DB`)**.
- This machine: enabled repos = `core extra multilib endeavouros` (`/etc/pacman.conf`); local DB = `linux` only. Tree today = 6 rows (core/linux, core/linux-lts, extra/linux-zen, extra/linux-hardened, extra/linux-rt, extra/linux-rt-lts). **Missing from the list: 15 of the 21 curated kernels** — 7× cachyos (`[cachyos]` not enabled), 7× chaotic-aur (mainline, xanmod×4, clear; repo not enabled, AUR path compiled out), 1× liquorix (lqx; repo not enabled, AUR compiled out). `linux-tkg` is build-only (no precompiled row by design).
- Install column: `km-window.cpp:116` `km::is_installable(name) ? "✓" : "—"`. `km::is_installable` (known_kernels.cpp:288-302) is **purely static table data** (`precompiled_available && !install_package.empty()`) — no alpm, no availability check → ✓ on 20/21 curated names regardless of system state. Header label is `"Install"` (km-window.ui:84-88); `TreeCol{Check=0,PkgName=1,Version=2,Category=3,Install=4,Immutable=5}` (km-window.hpp:90-97), `hideColumn(Immutable)`.
- `utils::is_package_available(pkg, repo)` (alpm_utils.cpp:103-121) already exists and is test-covered (k7: 27/27): AUR ⇒ `paru --aur -Si` probe (graceful false without paru); pacman repo ⇒ fresh `parse_alpm` + sync-DB name-match + `alpm_db_get_pkg`.
- `Kernel::is_installed()` (kernel.cpp:157-162) = local-DB lookup by `m_name` — **works even with `m_pkg == nullptr`** (name + `alpm_get_localdb` only); `version()`/`install()`/`remove()` dereference `m_pkg`/`m_headers` (would crash on a pkg-less row).
- Context menu (km-window.cpp:434-511): "Install pre-compiled %1" gated on static `km::is_installable` → for a curated kernel in a disabled repo the action is enabled but would fail at `pacman -S`.
- Tests asserting the static semantics: `tests/test_chunk2_known_kernels.cpp:229-238` (`is_installable` == table flags, incl. per-entry loop) — these stay green only if `km::is_installable` is **not** redefined.

---

## DECISIONS

### D1 (Objective 1) — per-repo JSON entries + codegen'd resolver; NO shared/default set
- **Chosen: (a) explicit per-repo entries in `compile_options.json`** for every repo whose PKGBUILDs actually consume the options: 6 CachyOS AUR-sibling keys (full 17-var map, verified identical convention) + 4 XanMod keys (`cpu_opt`→`_microarchitecture`, `localmodcfg`→`_localmodcfg`) + 1 Liquorix key (`localmodcfg`). Official-Arch, mainline, clear, tkg get **no entries** (no env switches / own framework — audit + fetched PKGBUILDs).
- **Rejected (b) shared/default set:** injecting `_cc_harder`/`_HZ_ticks`/… into PKGBUILDs that never read them is a silent no-op — rows would light up and promise effects that don't happen (the UI would lie). Row visibility must mean "the selected source's PKGBUILD consumes this variable".
- **Tick rate for other kernels:** no supported non-CachyOS PKGBUILD consumes HZ/tickless/preempt vars, so "tick rate for other kernels" is not achievable without misrepresenting the build; the expansion gives each kernel the options its packaging actually supports (CPU optimizations → XanMod `_microarchitecture`; modprobed-db → XanMod+LQX; the full 17-var suite → all 10 CachyOS flavors incl. the 6 previously-broken sibling sources). This is documented per-repo in E1.
- **Value translation:** XanMod `_microarchitecture` is a numeric menu and `_localmodcfg` is y/n; LQX `_localmodcfg` is non-empty/empty. The JSON value becomes an object `{var, values{src→dst}}`; codegen emits one flat frozen map `detail::value_xforms` keyed `"<repo>|<option>|<value>"`; `get_all_set_values` applies the translation uniformly to combo (and checkbox) emissions (no entry ⇒ pass-through). Untranslatable UI values (x86_64_v2/v3/v4 on XanMod) emit nothing + stderr note, exactly like today's `"manual"` behavior.
- **Resolver codegen:** the hand-written `if (repo_key == "linux_cachyos")` becomes a generated `detail::resolve_option_map` returning `std::optional<std::variant<const Map1*, …, const Map12*>>` (frozen maps with different sizes are different types ⇒ variant). All key knowledge lives in the JSON; C++ carries no key list.
- **Known limitation (out of scope, flagged for follow-up):** the flavor-level preempt combo offers Voluntary/None for `lts`+`hardened` (conf-window.cpp:884-891), but the live hardened PKGBUILD's `_preempt` case accepts only full|lazy (lts accepts all four) → building hardened with Voluntary/None would `_die` in the PKGBUILD. Pre-existing; not touched this cycle.

### D2 (Objective 2) — root cause = uninstalled-run fallback + .ui null-override; fix the wiring, keep the dedicated artwork
- **Root cause (two independent defects):** (i) code — uic-generated per-window `setWindowIcon(QIcon::fromTheme("org.archlinux.KernelManager"))` yields a **null** icon when the hicolor icon isn't installed and **overrides the bundled app icon** on both windows; (ii) environment — a build-dir run has no installed hicolor icons and no `.desktop`, so the panel can't resolve `org.archlinux.KernelManager` and shows the theme placeholder — on this system the Qogir `wayland` icon = the **W**. The PNG assets themselves are the dedicated (upstream-inherited) green artwork, verified not-W (see current state).
- **Fix (code):** remove the `windowIcon` property from **both** .ui files; make the bundled resource **multi-size** (distinct `:/km-icons/48x48.png` + `:/km-icons/256x256.png` aliases); build one multi-size `QIcon` in `main.cpp` as the app default; and set **`setWindowIcon(QApplication::windowIcon())` explicitly in both window constructors** (defense in depth — a window icon is guaranteed regardless of Qt app-fallback semantics or future .ui edits). CMake/PKGBUILD install rules are already correct and stay.
- **Fix (environment, ops step E11):** install the package (or `cmake --install`) so `/usr/share/applications/org.archlinux.KernelManager.desktop` + the 10 hicolor icons exist, refresh the icon cache, and verify on the live desktop that **taskbar AND window** show the green icon (W gone). For end users who install the package, the W never appears.
- **User-input flag (proceeding on best judgment):** the dedicated artwork is the **upstream-inherited green icon**. If the user wants a *different* dedicated mark (e.g. Arch-themed), that is a **new asset** this cycle cannot design; the plan keeps the existing green artwork and fixes all wiring so *any* replacement drop-in (same file names/aliases) inherits the fix automatically.

### D3 (Objective 3a) — curated info-rows complete the list (build-only tkg excluded)
- Missing kernels are missing because the list is **repo-driven by design** (rows = packages present in enabled sync DBs / AUR) while the 15 curated kernels live in repos not enabled on this system (and the AUR path is compiled out). Users cannot install from a disabled repo, but the app **can** tell them the kernel exists, which repo it needs, and that it isn't installed.
- **Decision:** `get_kernels` appends, after the repo/AUR passes, **info-rows** for every curated `precompiled_available` kernel whose name isn't already listed: `Kernel{handle, /*pkg*/nullptr, /*headers*/nullptr, install_repo, "<repo>/<name>"}`. Info-rows render version `"—"` (guarded `version()`), **non-user-checkable** Choose cell (cannot be selected for install/remove), and a PkgName-tooltip suffix stating the reason (repo not in `/etc/pacman.conf` vs. repo registered but package absent from its DB ⇒ `pacman -Sy`). `linux-tkg` (no precompiled package) stays out of the tree (reachable via Build-custom/Configure — unchanged).
- `install()`/`remove()` get `m_pkg==nullptr ⇒ return false` guards (no crash paths); `is_installed()` needs no change (name + local DB only — an info-row shows ✓ if the user previously installed that kernel from a since-disabled repo, which is correct).

### D4 (Objective 3b) — Install column semantics = **(B) "installed on the system"**
- **Decision:** `Install` cell = `kernel.is_installed() ? "✓" : "—"` (alpm **local DB**, name-based); header renamed **`Installed`**; `km::is_installable` is **left unchanged** (static "has a documented precompiled path" — still gates the context menu, still satisfies `test_chunk2_known_kernels.cpp:229-238`).
- **Justification:** (1) the user's literal words say "actually **installed**"; (2) interpretation (A) "available in repos/AUR" is a **no-op visually** — every row the list can produce already comes from an enabled repo or the AUR, so (A) would still print ✓ on all rows and could not fix the "checkmark for all entries regardless of status" complaint; (3) installed-state is the only per-row truth that varies (on this machine: `linux` ✓, other 20 —); (4) the column label "Install" on a per-row status reads naturally as installed-state, and the Choose checkbox keeps meaning "selected for change".
- **Alternative noted:** (A) becomes meaningful **only together with** the 3a info-rows (curated kernels from disabled repos are "not installable right now"); the context menu therefore gets the availability truth: "Install pre-compiled %1" is enabled iff `km::is_installable(name)` **and** `utils::is_package_available(install_package, install_repo)` (graceful false without paru / disabled repo), so no action promises a failing pacman run. The in-list ✓/— of the `Installed` column reports installed-state; the menu reports installability. Both are true; neither duplicates the other.

---

## EXECUTION CHUNKS

Lane keys (same-file serialization): **L-json** E1; **L-codegen** E2; **L-conf** E3→E10 (both `src/conf-window.cpp`); **L-km** E9→E14 (both `src/km-window.cpp`); **L-kmui** E7→E15 (both `src/km-window.ui`); **L-main** E5→E6; **L-ui2** E8; **L-kernel** E12→E13; **L-tests** E4, E17 (disjoint files, parallel-safe); **L-kmhpp** E16.

### E1 — compile_options.json: 11 new per-repo entries (Obj 1)
- **Objective:** 1 · **File:** `src/compile_options.json` (single) · **Dep:** `[CRITICAL-PATH]` (forks from main; foundation for E2/E3) · **Lane:** L-json.
- **Spec:** keep `linux_cachyos` byte-identical. Add entries:
  - `linux_cachyos_bore`, `linux_cachyos_rt_bore`, `linux_cachyos_lts`, `linux_cachyos_server`, `linux_cachyos_deckify`, `linux_cachyos_bmq` — each the **same 17-var map** as `linux_cachyos` (verified identical convention across the `CachyOS/linux-cachyos` repo flavors and the AUR sibling packages).
  - `linux_xanmod`, `linux_xanmod_edge`, `linux_xanmod_lts`, `linux_xanmod_rt` — each: `"cpu_opt": {"var": "_microarchitecture", "values": {"native": "99", "generic_v1": "0", "zen4": "14"}}` (XanMod numeric menu; v2/v3/v4 have no menu equivalent ⇒ unemitted, D1) and `"localmodcfg": {"var": "_localmodcfg", "values": {"yes": "y", "no": "n"}}`.
  - `linux_lqx` — `"localmodcfg": {"var": "_localmodcfg", "values": {"yes": "yes", "no": ""}}` (non-empty ⇒ enabled; empty disables).
  - Keys must stay C-identifier-safe (they are: `normalize_repo_key` maps `-`→`_`; AUR names and the tkg URL basename normalize to these keys / to no entry respectively).
- **Acceptance:** `python3 -m json.tool` clean; `linux_cachyos` object unchanged; 12 top-level keys total; every new key present exactly once.

### E2 — mkoptions.py: object values + generated resolver + value_xforms (Obj 1)
- **Objective:** 1 · **File:** `src/mkoptions.py` (single) · **Dep:** `[COUPLED-TO: E1]` · **Lane:** L-codegen.
- **Spec:** extend the generator (header template otherwise unchanged; add `#include <optional>` + `#include <variant>`):
  1. Per repo entry: value that is a **string** ⇒ var-map pair `option→var` (as today); value that is an **object** ⇒ var-map pair `option→obj["var"]` **plus**, for each `obj["values"]` pair, one entry in a new **global flat** `detail::value_xforms` frozen map keyed `frozen::string{repo + "|" + option + "|" + src}` → `dst`.
  2. New generated `detail::option_maps_variant = std::variant<const <map1>*, …, const <map12>*>` and `inline std::optional<detail::option_maps_variant> detail::resolve_option_map(std::string_view repo)` — generated if-chain (`repo == "<key>" ⇒ in_place_index + &detail::<key>`), `std::nullopt` default.
  3. `needs_run()` unchanged (JSON mtime already a dependency).
- **Acceptance:** regenerate into a scratch dir; header `g++ -std=c++23 -fsyntax-only` (frozen includes) clean, 0 warnings; `detail::resolve_option_map("linux_cachyos")` has value and finds all 17 vars; `("linux_xanmod")` finds exactly `cpu_opt`+`localmodcfg`; `("linux")` ⇒ nullopt; `value_xforms` contains exactly the E1 translation entries (spot-check `linux_xanmod|cpu_opt|native`→`99`, `linux_lqx|localmodcfg|no`→``).

### E3 — conf-window.cpp: generated resolver + value translation (Obj 1)
- **Objective:** 1 · **File:** `src/conf-window.cpp` (single) · **Dep:** `[COUPLED-TO: E2]` · **Lane:** L-conf (E10 comes after this merges).
- **Spec:** (a) delete the hand-written `resolve_option_map` (:282-293, incl. `option_map_type`/`option_map_ref` aliases) — `option_map_ref` becomes `std::optional<detail::option_maps_variant>`; call `detail::resolve_option_map` at both sites (:576, :671). (b) `option_build_var` becomes a `std::visit` over the variant with the same nullopt contract (absent map OR absent option ⇒ nullopt). (c) new file-local `xformed_value(std::string_view repo, std::string_view option, std::string_view value)`: build `"<repo>|<option>|<value>"`, look up `detail::value_xforms` (frozen::string from string_view), return the translation or pass through; on a translation that maps to the **empty** string and the option is a combo, emit nothing (mirrors the existing `"manual"` skip) with a one-line stderr note. (d) `get_all_set_values`: capture `repo_key = normalize_repo_key(utils::build_source_repo())` once; wrap every combo value emission (6 lines :586-594) and every checkbox emission (:581-583, via `convert_to_var_assign*`) in `xformed_value(repo_key, …)`; keep the LTO `custom_name` workaround (:599-604) as-is. `update_option_set` unchanged.
- **Acceptance:** clean compile, 0 new warnings; offscreen driver (E4 harness) shows: source `linux-cachyos` ⇒ 17/17 rows visible; `linux-cachyos-bore` ⇒ 17/17; `linux-xanmod` ⇒ exactly `cpu_opt`+`localmodcfg` visible (15 hidden); `linux-lqx` ⇒ exactly `localmodcfg` visible; `linux` and any custom unknown ⇒ 0 visible; `get_all_set_values` with source `linux-xanmod`, cpu_opt=native, localmodcfg on ⇒ output contains exactly the lines `_microarchitecture=99` and `_localmodcfg=y` (no other vars); source `linux-cachyos`, hzticks=300, bbr3 on ⇒ contains `_HZ_ticks=300`, `_tcp_bbr3=yes`.

### E4 — test_chunk2_conf_window.cpp: per-source row-visibility + emission harness (Obj 1)
- **Objective:** 1 · **File:** `tests/test_chunk2_conf_window.cpp` (single; `tests/run_chunk2_ui.sh` unchanged — it already compiles this test + `conf-window.cpp` with `-I$BUILD` for the generated header) · **Dep:** `[COUPLED-TO: E3]` · **Lane:** L-tests.
- **Spec:** extend the existing offscreen driver (keep all 13 current assertions): after each `conf.set_build_source(<src>)`, assert the visible/hidden state of the 17 `option_row_bindings` widgets (`isHidden()` on the `*_widget` row objects) per E3's acceptance matrix; assert the `get_all_set_values()` emissions for `linux-xanmod` (native/localmodcfg) and `linux-cachyos` (hz 300/bbr3) exactly as specified in E3.
- **Acceptance:** `tests/run_chunk2_ui.sh` all green (13 pre-existing + new), exit 0.

### E5 — km_icons.qrc: distinct multi-size aliases (Obj 2)
- **Objective:** 2 · **File:** `src/km_icons.qrc` (single) · **Dep:** `[ISOLATED]` (forks from main) · **Lane:** L-main.
- **Spec:** replace the two duplicate-alias entries with distinct aliases: `<file alias="48x48.png">../icons/48x48/org.archlinux.KernelManager.png</file>` and `<file alias="256x256.png">../icons/256x256/org.archlinux.KernelManager.png</file>` (prefix `/km-icons` unchanged); rewrite the comment (multi-size master pair; no last-wins dedupe; installed use still resolves the hicolor theme).
- **Acceptance:** CMake AUTORCC regenerates the rcc output; both resource paths present in the linked binary (probe: a throwaway `QIcon(":/km-icons/48x48.png")` + `QIcon(":/km-icons/256x256.png")` both non-null, sizes 48/256).

### E6 — main.cpp: multi-size app window icon (Obj 2)
- **Objective:** 2 · **File:** `src/main.cpp` (single) · **Dep:** `[COUPLED-TO: E5]` · **Lane:** L-main.
- **Spec:** replace the single-path `QIcon` construction (:137-143) with a multi-size icon: `QIcon window_icon; window_icon.addFile(QStringLiteral(":/km-icons/48x48.png")); window_icon.addFile(QStringLiteral(":/km-icons/256x256.png"));` keep the `isNull()` stderr notice + `app.setWindowIcon(window_icon);` (before `MainWindow`, as today). `setDesktopFileName` untouched.
- **Acceptance:** clean build; offscreen smoke alive (exit 124, empty stderr); throwaway probe: `QApplication::windowIcon()` non-null, `availableSizes()` ⊇ {48, 256}.

### E7 — km-window.ui: drop the windowIcon theme property (Obj 2)
- **Objective:** 2 · **File:** `src/km-window.ui` (single) · **Dep:** `[ISOLATED]` (forks from main) · **Lane:** L-kmui (E15 after this merges).
- **Spec:** delete the `<property name="windowIcon"> … <iconset theme="org.archlinux.KernelManager"/> … </property>` block (km-window.ui:19-21) from the `MainWindow` widget definition. No other property touched.
- **Acceptance:** regenerated `ui_km-window.h` contains **no** `fromTheme`/`setWindowIcon`; `run_chunk2_ui.sh`-style smoke still alive.

### E8 — conf-window.ui: drop the windowIcon theme property (Obj 2)
- **Objective:** 2 · **File:** `src/conf-window.ui` (single) · **Dep:** `[ISOLATED]` (forks from main) · **Lane:** L-ui2.
- **Spec:** delete the `<property name="windowIcon"> … <iconset theme="org.archlinux.KernelManager"><normaloff>.</normaloff></iconset> … </property>` block (conf-window.ui:19-22).
- **Acceptance:** regenerated `ui_conf-window.h` contains **no** `iconThemeName`/`addFile`/`setWindowIcon`; `tests/run_chunk2_ui.sh` green (it compiles the page/window mocs — the removed property must not break uic).

### E9 — km-window.cpp: explicit per-window icon (Obj 2)
- **Objective:** 2 · **File:** `src/km-window.cpp` (single) · **Dep:** `[COUPLED-TO: E6, E7]` · **Lane:** L-km (E14 after this merges).
- **Spec:** in `MainWindow`'s constructor, immediately after `m_ui->setupUi(this);` (:157): `setWindowIcon(QApplication::windowIcon());` (+ one-line comment: explicit dedicated icon; the .ui no longer overrides; robust to Qt app-fallback semantics). `#include <QApplication>` already available via Qt6::Widgets umbrella — verify no new include needed (add if the build says so).
- **Acceptance:** clean build, 0 new warnings; offscreen smoke alive; throwaway driver: `MainWindow`'s `QWindow::icon()` non-null with sizes {48, 256} even when no icon theme contains `org.archlinux.KernelManager`.

### E10 — conf-window.cpp: explicit per-window icon (Obj 2)
- **Objective:** 2 · **File:** `src/conf-window.cpp` (single) · **Dep:** `[COUPLED-TO: E6, E8]` · **Lane:** L-conf (forks after E3 merges — same file).
- **Spec:** in `ConfWindow`'s constructor, immediately after `m_ui->setupUi(this);` (:768): `setWindowIcon(QApplication::windowIcon());` (+ same comment pattern as E9).
- **Acceptance:** same as E9 for the ConfWindow; `tests/run_chunk2_ui.sh` green (ConfWindow is instantiated there).

### E11 — ops: install + icon-cache + live-desktop verification (Obj 2)
- **Objective:** 2 · **File:** none (no commit; SysOps step) · **Dep:** `[COUPLED-TO: E6..E10 merged]` · **Lane:** —.
- **Spec:** on the dev machine: `cmake --install build --prefix /usr` (or `makepkg -si` the PKGBUILD) ⇒ `/usr/share/applications/org.archlinux.KernelManager.desktop` + 10 `hicolor/*/apps/org.archlinux.KernelManager.png`; refresh caches (`update-desktop-database /usr/share/applications`; `gtk-update-icon-cache`/KDE icon cache as applicable); log out/in; launch `./build/kernel-manager`; **verify taskbar AND window titlebar show the dedicated green icon (W gone)**. If the user's desktop still shows the W, capture the panel's resolved icon name for follow-up (do not block the cycle on a compositor cache quirk — document it).
- **Acceptance:** both surfaces show the green icon post-install; documented in the cycle archive.

### E12 — kernel.hpp: has_pkg accessor (Obj 3)
- **Objective:** 3 · **File:** `src/kernel.hpp` (single) · **Dep:** `[CRITICAL-PATH]` (forks from main) · **Lane:** L-kernel.
- **Spec:** add public `constexpr bool has_pkg() const noexcept { return m_pkg != nullptr; }` near the other accessors + a class-comment paragraph defining the two row kinds: (1) live rows (repo/AUR, `m_pkg` set — today's behavior), (2) **curated info-rows** (`m_pkg == nullptr`: a known-kernels entry not present in any enabled repo/AUR — display-only, not selectable for install/remove; `version()` yields `"—"`, `is_installed()` still valid via local-DB name lookup).
- **Acceptance:** header compiles in the Release build (0 new warnings); accessor is `[[nodiscard]]`-free constexpr (matches file style).

### E13 — kernel.cpp: guards + curated info-row append (Obj 3)
- **Objective:** 3 · **File:** `src/kernel.cpp` (single) · **Dep:** `[COUPLED-TO: E12]` · **Lane:** L-kernel.
- **Spec:** add `#include "known_kernels.hpp"`. (a) `version()`: first line `if (m_pkg == nullptr) { return "—"; }`. (b) `install()` and `remove()`: first line `if (m_pkg == nullptr) { return false; }` (remove() already early-returns on `!is_installed()`; keep both guards for crash-safety regardless of call order). (c) `get_kernels`: after the existing repo pass (and after the `#ifdef ENABLE_AUR_KERNELS` AUR pass), append curated info-rows: iterate `km::known_kernels()`; skip entries with `!precompiled_available` (tkg) and entries whose `name` is already present in `kernels` (by `m_name` — a repo/AUR row wins); otherwise `kernels.emplace_back(Kernel{handle, nullptr, nullptr, std::string_view{e.install_repo}, fmt::format(FMT_COMPILE("{}/{}"), e.install_repo, e.name)});` (5-arg ctor exists at kernel.hpp:35).
- **Acceptance:** on this machine `get_kernels` ⇒ 6 live rows (all `has_pkg()`) + 15 info-rows (`has_pkg()==false`, `version()=="—"`, raw `"<repo>/<name>"`, e.g. `cachyos/linux-cachyos`, `chaotic-aur/linux-xanmod`, `liquorix/linux-lqx`); no duplicate names; `linux-tkg` absent; `install()`/`remove()` on an info-row return false without touching globals; clean build 0 new warnings.

### E14 — km-window.cpp: Installed column + truthful menu + info-row flags (Obj 3)
- **Objective:** 3 · **File:** `src/km-window.cpp` (single) · **Dep:** `[COUPLED-TO: E9, E13]` (same-file lane after E9; logic after E13) · **Lane:** L-km.
- **Spec:** (a) `init_kernels_tree_widget` :116 — `widget_item->setText(TreeCol::Install, kernel.is_installed() ? QStringLiteral("✓") : QStringLiteral("—"));` (+ update the K10 comment block above it to the installed-state semantics, D4). (b) Row flags :117 — live rows keep `Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable`; info-rows (`!kernel.has_pkg()`) get `Qt::ItemIsEnabled | Qt::ItemIsSelectable` only (Choose cell disabled) and their PkgName tooltip gains a suffix: if `install_repo` is registered among the handle's sync DBs ⇒ `" — not in the '<repo>' DB (pacman -Sy)"` else ⇒ `" — repo '<repo>' not enabled (add it to /etc/pacman.conf)"` (small file-local helper scanning `alpm_get_syncdbs` names; `#include` none new — alpm.h already via kernel.hpp). (c) `on_kernel_context_menu` :458 — `install_action->setEnabled(km::is_installable(name) && is_precompiled_available_live(name))` where `is_precompiled_available_live` (file-local) = `if (auto e = km::find_kernel(name)) return e->precompiled_available && !e->install_package.empty() && utils::is_package_available(e->install_package, e->install_repo); return false;` (`#include "alpm_utils.hpp"` — utils.hpp already includes it; verify). Menu text/flow otherwise unchanged.
- **Acceptance:** on this machine the tree has 21 rows; the `Installed` column is ✓ on exactly the rows whose package is in the local DB (`linux` only) and — on the other 20; all 15 info-rows have a disabled Choose cell + repo-reason tooltip; context menu "Install pre-compiled linux-cachyos" is **disabled** (repo not enabled) while "Install pre-compiled linux" is **enabled** (core DB has it); "Build custom" + "Show boot instructions" unchanged for all rows; `build_change_list`/`item_changed` byte-identical (disjoint regions); clean build, 0 new warnings.

### E15 — km-window.ui: header "Install" → "Installed" (Obj 3)
- **Objective:** 3 · **File:** `src/km-window.ui` (single) · **Dep:** `[COUPLED-TO: E7]` (same-file lane after E7 merges) · **Lane:** L-kmui.
- **Spec:** the tree column header string `Install` (km-window.ui:84-88) → `Installed` (1-line text change; uic retranslate keeps all six header sections).
- **Acceptance:** regenerated `ui_km-window.h` header array shows `Installed` in the 5th slot; smoke alive. i18n: `Installed` is a translatable .ui string — **lupdate resync deferred** per the K10/chunk-5 precedent (stale .ts falls back to source text; noted for the optional lupdate pass).

### E16 — km-window.hpp: TreeCol comment refresh (Obj 3)
- **Objective:** 3 · **File:** `src/km-window.hpp` (single) · **Dep:** `[COUPLED-TO: E14]` · **Lane:** L-kmhpp.
- **Spec:** update the `Install` enum comment (:95) from "pre-compiled install availability indicator (✓/—), read-only" to "installed-on-system indicator (✓/—) from the alpm local DB, read-only; availability is the context-menu's concern" (comment only; enum values/order untouched).
- **Acceptance:** compiles; no behavioral delta.

### E17 — tests/test_k11_tree_rows.cpp + run_k11.sh: get_kernels info-row harness (Obj 3)
- **Objective:** 3 · **Files:** `tests/test_k11_tree_rows.cpp` (new) + `tests/run_k11.sh` (new) — one test unit, K8 precedent; no CMakeLists change (standalone harness compiles the real sources) · **Dep:** `[COUPLED-TO: E13]` · **Lane:** L-tests.
- **Spec:** harness (k7 recipe: g++ C++23 + full project warning set; compile `kernel.cpp`, `known_kernels.cpp`, `utils.cpp`, `alpm_utils.cpp`, `bootloader.cpp`? — no, only what `kernel.cpp` needs: `utils.cpp` + `alpm_utils.cpp` + fmt + `-lalpm`; offscreen not required since no Qt): (1) every row from `Kernel::get_kernels(parse_alpm(...))` has `has_pkg()` consistent with its origin (repo rows true); (2) info-rows = exactly `curated precompiled_available − names present in repo rows`, each `has_pkg()==false`, `version()=="—"`, `get_raw()=="<install_repo>/<name>"`; (3) no duplicate `m_name` in the full list; (4) `is_installed()` on an info-row equals a direct `alpm_db_get_pkg(localdb, name) != nullptr` probe (null-pkg path is crash-free and truthful); (5) `install()`/`remove()` on an info-row return false and leave `get_install_list()`/`get_removal_list()` untouched. Environment-tolerant per the k7 precedent (assert against the actual local/sync DBs, never hard-coded counts).
- **Acceptance:** `tests/run_k11.sh` green exit 0 on this machine (expected: 6 live + 15 info rows; `linux` installed-true; the other info-rows installed-false).

---

## SCHEDULE (sliding window size 2; each window dispatches after the prior window's merges land on main; fork = current main unless COUPLED-TO says otherwise)

| Window | Slot A | Slot B | Notes |
|---|---|---|---|
| W1 | **E1** (json) `[CRITICAL-PATH]` | **E5** (qrc) `[ISOLATED]` | disjoint files/lanes |
| W2 | **E2** (mkoptions) ←E1 | **E6** (main.cpp) ←E5 | |
| W3 | **E3** (conf.cpp) ←E2 | **E7** (km-window.ui) `[ISOLATED]` | L-conf starts; L-kmui starts |
| W4 | **E4** (conf test) ←E3 | **E8** (conf-window.ui) `[ISOLATED]` | obj-1 functionally complete after E4 merges |
| W5 | **E9** (km.cpp icon) ←E6,E7 | **E12** (kernel.hpp) `[CRITICAL-PATH]` | L-km starts; L-kernel starts |
| W6 | **E10** (conf.cpp icon) ←E6,E8 (after E3) | **E13** (kernel.cpp) ←E12 | L-conf closes (obj-2 code complete after E10) |
| W7 | **E14** (km.cpp column/menu) ←E9,E13 | **E15** (km-window.ui header) ←E7 | L-km closes; L-kmui closes; obj-3 functionally complete after E14 |
| W8 | **E16** (km.hpp comment) ←E14 | **E17** (k11 harness) ←E13 | |
| W9 | **Merge wave** (all reviewed chunks, `--no-ff`, in lane order) | **E11 ops** (install + icon-cache + live verification) | QA audit on merged main; cycle archive |

- File-conflict invariant: at no window do two active branches touch the same file (L-conf: E3→E10 serialized; L-km: E9→E14 serialized; L-kmui: E7→E15 serialized; L-main: E5→E6; L-kernel: E12→E13).
- Review cadence: every chunk reviewed before merge (purity = scoped files only; warning set == baseline; smoke alive) per the `kernel-manager-kernels` precedent.

## Definition of Done
1. **Build:** from-scratch `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)` exit 0; **exactly 1 warning** = pre-existing `src/utils.cpp:103` `-Wignored-attributes`; 0 new.
2. **Harnesses:** `run_chunk1.sh` 34/34, `run_chunk2.sh` 129/129 (`km::is_installable` semantics untouched), `run_chunk2_ui.sh` 13 + E4 additions green, `run_k5.sh` 26/26, `run_k6.sh` 37/37, `run_k7.sh` 27/27, `run_k8.sh` 75/75, `run_k11.sh` green (E17) — all exit 0, ≥ 341 + new assertions, no regressions.
3. **Smoke:** `QT_QPA_PLATFORM=offscreen timeout 12 ./build/kernel-manager` ⇒ exit 124 (alive), empty stderr.
4. **Behavior:** Configure page offers the full 17-var suite for `linux-cachyos` **and** all 6 sibling sources; XanMod×4 offer cpu-opt (translated) + modprobed-db; LQX offers modprobed-db; official/mainline/clear/tkg offer none (documented reason). Window + taskbar show the dedicated green icon after install (W gone); multi-size resource verified. Tree shows 21 rows on this machine (6 live + 15 info); `Installed` column truthful vs local DB; info-rows non-selectable with repo-reason tooltips; context-menu install action truthful (disabled for unenabled repos, graceful without paru).
5. **Follow-ups (documented, not in this cycle):** lupdate resync for the `Installed` header string (K10 precedent); PKGBUILD `_commit` re-pin + sha256 after the final merge (carried from `kernel-manager-kernels`, needs a release tag — user input); hardened-flavor preempt Voluntary/None vs live PKGBUILD `_die` mismatch (D1 note); optional replacement of the upstream-inherited icon asset (D2 note — user input).
