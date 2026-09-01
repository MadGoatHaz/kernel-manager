# Changelog

### v1.23.0 (2026-08-31) — release candidate

[Full Changelog](https://github.com/MadGoatHaz/kernel-manager/compare/aad790e...f8536cc)

- 🖼️ **Custom project icon:** an amber gauge mark (scx-manager geometry, `#F57F17`) replaces the inherited CachyOS green artwork — all 10 hicolor sizes + the root 390×390 regenerated, with the qrc/CMake/.desktop wiring unchanged (name-based inheritance)
- 🔥 **Repo-add action:** new right-click "Add repo '<repo>'" for kernels whose pacman repo isn't enabled — `pkexec` `auth_admin` escalation, idempotent append to `/etc/pacman.conf` with a timestamped backup written first, then a `pacman -Sy` refresh; the kernel list auto-refreshes and the row goes live
- ⚡ **Lock-glyph indicator:** a non-interactive lock icon replaces the confusing disabled checkbox on non-installable kernel rows, with a per-case tooltip (repo disabled vs. package missing from the DB)
- ℹ️ **Version annotation:** the version column now shows "— (repo not enabled)" for info-rows whose repo is disabled, distinct from a genuinely-missing version (bare "—")
- 📦 **Release:** the `v1.22.0` tag is cut at `aad790e` and the PKGBUILD re-pinned to it (`pkgver` 1.22.0)

> Note: release candidate only — 989/989 unit assertions green across all 10 harnesses (incl. the new k12 tree-render + k13 repo-add harnesses), clean build with 1 pre-existing baseline warning, 0 defects. An optional `lupdate` resync of the locale catalogs for the new `tr()` strings ("Add repo '%1'" + the confirm/refresh texts) is deferred. The v1.23.0 tag + `pkgver` bump happen at the next release cut. The official-kernel build-options UI remains deferred (all 6 official PKGBUILDs consume zero user build variables — toggles would be silent no-ops).

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
