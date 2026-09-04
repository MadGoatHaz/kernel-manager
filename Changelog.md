# Changelog

### v1.26.0 (2026-09-04)

[Full Changelog](https://github.com/MadGoatHaz/kernel-manager/compare/045feca...12693a4)

#### Changed
- **Install flow simplified** — the install now relies on the distro's own alpm hooks (NVIDIA DKMS, `kernel-install`/dracut) for all post-install work instead of the app's own tail script. One code path for every install.
- **Install result simplified to two states** — an install is **Installed** (green) or **Failed** (red, with the real exit code). The terminal output is the source of truth for DKMS/initramfs details.

#### Added
- **App version displayed in the UI** — the running version now appears in the window title and a permanent status-bar label (auto-tracks the CMake project version).

#### Removed
- The app no longer invokes `postinstall_tail.sh` during install (the script is kept in the tree as a manual repair tool).
- The 3-state boot-safety verdict is replaced by the simpler 2-state result.

> Note: the distribution-awareness layer, the machine-state-robust k11 test expectation, and the CMake project-version correction (1.19.0 → 1.25.0) all shipped in v1.25.0 and are recorded in that entry — they are intentionally not repeated here.

### v1.25.0 (2026-09-03)

[Full Changelog](https://github.com/MadGoatHaz/kernel-manager/compare/97e505b...045feca)

- 🔥 **Consolidated post-install tail (driver + initramfs + verify + honest verdict):** every pre-compiled and directory install now ends in a single escalated `postinstall_tail.sh` step that (1) rebuilds the full nvidia family for the new kernel (`nvidia`/`nvidia-open`/`nvidia-lts`/`nvidia-open-lts` via guarded `pacman -U`, plus `nvidia-dkms`/`nvidia-open-dkms` and `zfs` via `dkms autoinstall -k <KVER>` — each sub-action `pacman -Q`-guarded and a no-op when absent), (2) regenerates the initramfs **for the new KVER** (`reinstall-kernels` → `dracut --force --kver` → `mkinitcpio -k`, only the first available tool runs) and deploys it into the systemd-boot ESP machine-id layout, and (3) verifies boot safety (nvidia module probed under `extramodules/` + `updates/dkms/` — the corrected path, per-KVER non-empty initramfs, BLS entry for systemd-boot, `grub-mkconfig` for GRUB); the old fire-and-forget `|| true` steps and the unconditional "SUCCESS" banner are gone
- ⚡ **Sentinel launch/rc protocol:** every terminal command — including the `pkexec`-escalated ones — now runs through a wrapper that records started/rc/done sentinels, so the app receives the command's *real* exit code (Polkit-rejected ⇒ 126, stuck ⇒ the done-timeout rc) instead of the old fire-and-forget "always 0" (H3 closed)
- 💬 **Honest 3-state install verdict + dialogs:** installs now report `BOOT_SAFE` (green), `INSTALLED_NOT_BOOT_SAFE` (amber — "do NOT reboot" + the `sudo reinstall-kernels` repair directive), or `INSTALLATION_FAILED` (red) as distinct dialogs, each raised to the front before the modal (H1); a failed tail can no longer be masked as success
- 🌐 **Distribution awareness:** a new Qt-free `distro` module reads `/etc/os-release` (`ID` + `ID_LIKE`) to classify Arch / EndeavourOS / Manjaro / CachyOS / Garuda — per-family initramfs tool preference, BLS-entries-dir probe, and a distro name for the UI — so post-install behavior is tuned to the distro rather than assuming bare Arch
- 📦 **Release:** CMake project version bumped from 1.19.0 to 1.25.0 (M1; the build-completion modal promptness M2 and flat build-dir M3 recon-confirmed), the `v1.25.0` tag is cut at `045feca` and the PKGBUILD is re-pinned to it (`pkgver` 1.25.0, `_commit` = the tag commit, `sha256sums` refreshed from the fetched archive — byte-stability verified across two fetches)
- 🐛 **Test robustness:** the k11 harness's `local/<name>` live-origin expectation now probes the real local DB (machine-state robust — no hard-coded row set), and the k8/k14 harnesses are re-specified to the single-tail contract with the 3-state verdict probes

> Note: released — the `v1.25.0` tag is cut at `045feca` (the fully-fixed main after the A→E2 postinstall/sentinel/verdict chain + the F/G/H isolated chunks) and the PKGBUILD is re-pinned to it (`pkgver` 1.25.0). Full QA on merged main: 15/15 harnesses green (1,286 observed checks / 0 FAIL — k11 81/0, k12 264/0 ×2 + stable dump, k16 35/0, k17 46/0, k18 50/50, chunk2 129/129), zero-code-warning forced full recompile, offscreen smoke 124/0B/0B, system-state gates green (`/etc/pacman.conf` + local DB + user config untouched, every mktemp sandbox trap-cleaned), 0 defects. Known limitation (tracked follow-up, not a release defect): the app still has no visible version in the UI — `PROJECT_VERSION` has no C++ consumer (no `--version` flag or about dialog); the package/tag/PKGBUILD chain is fully versioned.

### v1.24.0 (2026-09-01)

[Full Changelog](https://github.com/MadGoatHaz/kernel-manager/compare/c8ca7c1...97e505b)

- 🔥 **Install from directory:** a new "Install from directory…" row in the kernel list; right-clicking opens a folder picker, the app finds `*.pkg.tar.zst` packages and installs them via `pacman -U` with the post-install tail; locally-installed kernels absent from the enabled repos appear as `local/<name>` rows (uninstallable via the existing checkbox path)
- 📁 **Build directory selection:** a bottom-left path label + "Browse…" button; persists via QSettings; `build_repo_path()`/`build_app_path()`/`aur_pkgbuilds_path()` all honor the selection; default unchanged (`~/.cache/kernel-manager/pkgbuilds`)
- 🐛 **Release fixes:** the Execute button now works on the selected "Install from directory…" pseudo-row, the terminal-helper konsole launch race is eliminated with build-completion tracking in the Configure module, the `build_helper.sh` GPG key extraction handles 3 more failure patterns (the v1.23.0 `build-gpg` behavior kept as the first needle), a "Build dir:" caption is added to the working-directory selector, and the 1 pre-existing source warning is eliminated — the zero-warning baseline
- 📦 **Release:** the `v1.23.0` tag is cut at `0c918d4`; the PKGBUILD is re-pinned (`pkgver` 1.23.0)

> Note: released — the `v1.24.0` tag is cut at `97e505b` (the full post-RC state: the v1.24.0 RC `c8ca7c1` + this cycle's 5 fixes) and the PKGBUILD is re-pinned to it (`pkgver` 1.24.0, `sha256sums` refreshed from the fetched archive). 1163 unit checks green across all 14 harnesses (incl. the new k16 GPG-needle + k17 terminal-helper harnesses), zero-warning build, 0 defects. An optional `lupdate` resync of the locale catalogs for the new `tr()` strings (the RC's "Browse…", "Choose build directory", the directory-row tooltip, the boot-selection/critical dialog texts + the new `Build dir:` caption from the `.ui`) is deferred; the 5 fixes add no further `tr()` strings (the Execute-button change reuses an existing constant, the build-completion messages are `fmt` stderr diagnostics, not `tr()`).

### v1.23.0 (2026-08-31)

[Full Changelog](https://github.com/MadGoatHaz/kernel-manager/compare/aad790e...0c918d4)

- 🖼️ **Custom project icon:** an amber gauge mark (scx-manager geometry, `#F57F17`) replaces the inherited CachyOS green artwork — all 10 hicolor sizes + the root 390×390 regenerated, with the qrc/CMake/.desktop wiring unchanged (name-based inheritance)
- 🔥 **Repo-add action:** new right-click "Add repo '<repo>'" for kernels whose pacman repo isn't enabled — `pkexec` `auth_admin` escalation, idempotent append to `/etc/pacman.conf` with a timestamped backup written first, then a `pacman -Sy` refresh; the kernel list auto-refreshes and the row goes live
- ⚡ **Lock-glyph indicator:** a non-interactive lock icon replaces the confusing disabled checkbox on non-installable kernel rows, with a per-case tooltip (repo disabled vs. package missing from the DB)
- ℹ️ **Version annotation:** the version column now shows "— (repo not enabled)" for info-rows whose repo is disabled, distinct from a genuinely-missing version (bare "—")
- 📦 **Release:** the `v1.22.0` tag is cut at `aad790e` and the PKGBUILD re-pinned to it (`pkgver` 1.22.0)
- 🔧 **Post-RC hardening (included in this tag):** GPG key auto-import + trust in the repo-add flow (proactive import before every `pacman -Sy`, reactive key-ID and unknown-trust repair), the same auto-import in the makepkg build flow, Add-repo success judged by the actual repo state (not the terminal exit code), and post-build `pacman -U` with absolute paths (pkexec CWD reset)

> Note: released — the `v1.23.0` tag is cut at `0c918d4` (the full post-RC state: the v1.23.0 RC `f8536cc` + the 5 post-RC hardening fixes) and the PKGBUILD is pinned to it (`pkgver` 1.23.0, `sha256sums` refreshed). 989/989 unit assertions green across all 10 harnesses (incl. the new k12 tree-render + k13 repo-add harnesses), clean build with 1 pre-existing baseline warning, 0 defects. An optional `lupdate` resync of the locale catalogs for the new `tr()` strings ("Add repo '%1'" + the confirm/refresh texts) is deferred. The official-kernel build-options UI remains deferred (all 6 official PKGBUILDs consume zero user build variables — toggles would be silent no-ops).

### v1.22.0 (2026-08-31)

[Full Changelog](https://github.com/MadGoatHaz/kernel-manager/compare/4c66803...738e24c)

- 🔥 Configure module now offers the full CachyOS build-option suite (17 options) for the 6 CachyOS sibling sources (bore, rt-bore, lts, server, deckify, bmq), plus CPU-optimization + modprobed-db options for all 4 XanMod flavors and modprobed-db for Liquorix — data-driven per-repo, with automatic value translation where the target PKGBUILDs use a different value scheme
- 🖼️ The dedicated application icon now shows correctly on both the window titlebar and the system taskbar — the Wayland "W" placeholder is gone (bundled multi-size icon; the per-window theme-icon override that nulled it out is removed)
- ⚡ The main-window kernel list is complete: kernels that live in not-enabled repos now appear as curated info-rows (clearly marked non-selectable, with the reason — repo not enabled vs. package not in the repo), and the `Install` column is renamed **Installed** and now truthfully reports whether the kernel is installed on this system (pacman local DB); install availability is reported by the row's context menu instead

> Note: released — 476/476 unit assertions green across all 8 harnesses, clean build with 1 pre-existing baseline warning, 0 defects. An optional `lupdate` resync of the 16 locale catalogs for the new UI strings (e.g. the `Installed` header) is deferred. The v1.22.0 tag is cut at aad790e and the PKGBUILD pinned to it (pkgver 1.22.0).

### v1.21.0 (2026-08-31) — release candidate

[Full Changelog](https://github.com/MadGoatHaz/kernel-manager/compare/33fe35a...262e59a)

- 🔥 Kernel list expanded from 6 to 21 maintained Arch kernels (official, CachyOS, XanMod, TKG, Liquorix, Clear, mainline) — each now maps to its own build source; the old `linux` → `linux-cachyos` auto-populate special case is gone
- 🔥 New context menu on every kernel row (right-click): **Install pre-compiled** (pacman repo, or AUR via `paru`; fails gracefully if the repo or `paru` is missing), **Build custom** (existing Configure flow), **Show boot instructions**
- 🔥 Post-install boot selection: the bootloader is detected automatically (GRUB / systemd-boot / UKI / unknown) and the app shows numbered steps for selecting the new kernel at next boot (incl. the `/boot/vmlinuz-<pkg>` note)
- ⚡ Per-kernel install-availability indicator (✓/—) in the kernel list — build-only kernels (e.g. `linux-tkg`) are clearly marked

> Note: release candidate only — the AUR `PKGBUILD` source pin is still the pre-decoupling `19f1e06` and must be repointed to a current commit/tag (with a pkgver bump) before AUR submission; no version has been cut for this entry yet. An optional `lupdate` resync of the 16 locale catalogs for the new UI strings (context-menu labels, "Install" header) is pending.

### v1.20.0 (2026-08-30) — release candidate

[Full Changelog](https://github.com/MadGoatHaz/kernel-manager/compare/4e26423...148b961)

- 🐛 Fix custom-build runtime crash: working directory is restored after the source clone (CwdGuard), kernel flavor/build paths are absolute, and PKGBUILD insert is guarded against empty reads
- 🔥 Build source is now a dropdown of known kernel packages plus a "Custom URL…" field for any git URL, backed by a curated known-kernels table
- ⚡ Selecting a kernel auto-populates the build source (e.g. mainline `linux` → `linux-cachyos`); manual choices are never clobbered
- ℹ️ Kernel description tooltips on the package-name column for every kernel (curated or synthesized)
- 🖼️ App icon embedded as a Qt resource — window/taskbar icon shows without installation
- ♻️ The two "Cancel" buttons are now "Close"; all 16 translation catalogs resynced

> Note: release candidate only — the AUR `PKGBUILD` source pin is still the pre-decoupling `19f1e06` and must be repointed to a current commit/tag (with a pkgver bump) before AUR submission; no version has been cut for this entry yet.

### v0.9.1 (2022-01-27)

[Full Changelog](https://github.com/MadGoatHaz/kernel-manager/compare/v0.9.0...v0.9.1)

- 🗒️  Add more information about cachyos-km
- 🐛 Fix theming
- ⚡ Make it work without root privileges
- Add logo, desktop file
- Update configure.sh to use all CPU threads
- ♻  Update fmtlib and cleanup the source code
- 🐛 Change the behaviour of "execute button" and thread logic

### v0.9.0 (2022-01-23)

[Full Changelog](https://github.com/MadGoatHaz/kernel-manager/compare/f897e30d69055a7ba3d97e461e3b062a7577df86...v0.9.0)

🔥 It contains a few new features and various bug fixes.

- Build with Qt 6.
- Any Arch Linux based distros supported.
- Run heavywork on different thread.
- Use timer to update progress bar and status text
- Update kernel if already installed kernel is out-of-date
- Print backtrace on crash.
- Support GCC 12.
