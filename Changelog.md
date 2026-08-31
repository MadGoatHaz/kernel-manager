# Changelog

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
