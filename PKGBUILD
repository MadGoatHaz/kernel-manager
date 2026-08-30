# Maintainer: MadGoat <ghazlett@gmail.com>
#
# Distro-agnostic (generic Arch) STABLE packaging for kernel-manager.
#
# This package builds a FIXED, checksummed source snapshot (a single commit)
# rather than tracking a moving branch. See PKGBUILD-git for the rolling
# variant that tracks `main` (per D7).
#
# --------------------------------------------------------------------------
# VERSIONING
#   pkgver mirrors the project's own version (CMakeLists `project(... VERSION
#   1.19.0)`) and the fork's latest tag NUMBER (v1.19.0).
#
#   IMPORTANT: the v1.19.0 TAG is the pre-rebrand upstream code (it is an
#   ancestor of main, 11 commits behind, and predates the distro-agnostic
#   rename to `kernel-manager`). A v1.19.0 archive would therefore package the
#   WRONG (CachyOS-branded) content. The stable source below is instead pinned
#   to the rebranded commit 19f1e06 (= main HEAD at the chunk-5 merge, the
#   verified rebranded tree). When a proper release tag (e.g. v1.20.0) is cut
#   on the fork, bump pkgver and point `source` at that tag's archive.
#
# --------------------------------------------------------------------------
# DEPENDENCIES  (verified against `ldd` on the built binary + the CMake build)
#   depends (runtime):
#     qt6-base    -> libQt6Widgets/Gui/Core/Concurrent/DBus .so.6
#     pacman      -> libalpm.so.16
#     glib2       -> libglib-2.0.so.0
#     scx-manager -> libscxctl-ui.so.1 (sched-ext SCX control library, from
#                    the AUR package `scx-manager`)
#     polkit      -> pkexec, used by /usr/lib/kernel-manager/rootshell.sh for
#                    the privilege-escalation path (auth_admin polkit action)
#
#   makedepends (build time only):
#     cmake make gcc -> toolchain (C++23, Release, LTO). The build uses the
#                       default compiler (GCC), NOT clang/libc++ -- so
#                       llvm/libc++ are intentionally absent.
#     git          -> CPM fetches fmt/frozen/Corrosion from GitHub at configure
#     rust         -> cargo, for the Corrosion/cxx Rust crate (config-option-lib)
#     qt6-tools    -> lrelease, generates the .qm translations (embedded into
#                     the binary via km_locale.qrc -- NOT installed separately)
#     pkgconf      -> pkg-config, resolves libalpm.pc + glib-2.0.pc
#     python       -> Python3, runs src/mkoptions.py code generation
#     pacman glib2 -> headers/.pc for the build (same package is also a runtime
#                     dep; on Arch one package provides both)
#     scx-manager  -> scxctl-ui CMake config for find_package(scxctl-ui 1)
#     polkit-qt6   -> PolkitQt6-1 CMake config for find_package(PolkitQt6-1).
#                     Build-time only: the polkit-qt6 library is NOT linked into
#                     the binary (verified via ldd), so it is not a runtime dep.
#
#   fmt + frozen are CPM-vendored (fetched at configure time) -- build-only,
#   not runtime deps.
#
# --------------------------------------------------------------------------
# CONFLICTS / PROVIDES
#   This is the rebrand of the former `cachyos-kernel-manager` package. The
#   conflict + provide make it drop-in replace the CachyOS package cleanly.

pkgname=kernel-manager
pkgver=1.19.0
pkgrel=1
pkgdesc="Qt6 GUI for kernel configuration, compilation, and sched-ext (BPF) scheduler management"
arch=(x86_64)
url="https://github.com/MadGoatHaz/kernel-manager"
license=(GPL-3.0-or-later)

conflicts=(cachyos-kernel-manager)
provides=(cachyos-kernel-manager)

depends=(qt6-base pacman glib2 scx-manager polkit)
makedepends=(cmake make gcc git rust qt6-tools pkgconf python pacman glib2 scx-manager polkit-qt6)

# Defensive polkit reload so the shipped policy is picked up on (re)install.
install=kernel-manager.install

# Pinned rebranded commit (main HEAD at the chunk-5 merge). See the VERSIONING
# note above for why this is a commit, not the v1.19.0 tag.
_commit=19f1e0611452b0d497410a879270f98692d4733a
source=("https://github.com/MadGoatHaz/kernel-manager/archive/${_commit}.tar.gz")
sha256sums=("a9b3d29ee09b3275e62049fe72d78729bb0242ba9873145bb455cbb087be9dbe")

# GitHub archive top dir for a commit is `<repo>-<full-sha>`.
_srcdir="kernel-manager-${_commit}"

build() {
  # Fat LTO objects so makepkg can strip the LTO-built binary/static archives;
  # see https://archlinux.org/todo/lto-fat-objects/ (this project enables LTO
  # for Release builds in CMakeLists).
  CFLAGS+=" -ffat-lto-objects"
  CXXFLAGS+=" -ffat-lto-objects"

  cd "$srcdir/$_srcdir"
  cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr
  cmake --build build
}

package() {
  cd "$srcdir/$_srcdir"
  # Stages every artifact under $pkgdir/usr/...:
  #   binary            -> usr/bin/kernel-manager
  #   terminal-helper   -> usr/lib/kernel-manager/terminal-helper
  #   rootshell.sh      -> usr/lib/kernel-manager/rootshell.sh
  #   polkit policy     -> usr/share/polkit-1/actions/org.archlinux.kernel-manager.pkexec.policy
  #   desktop           -> usr/share/applications/org.archlinux.KernelManager.desktop
  #   10 icons          -> usr/share/icons/hicolor/<size>/apps/org.archlinux.KernelManager.png
  #
  # The helper + policy destinations are ABSOLUTE in CMakeLists
  # (KM_HELPER_DIR=/usr/lib/kernel-manager, POLKITQT-1_POLICY_FILES_INSTALL_DIR=
  # /usr/share/polkit-1/actions), so a bare `cmake --install --prefix` alone
  # would skip them. DESTDIR correctly prepends the staging dir to every
  # destination (relative AND absolute). The .qm translations are embedded into
  # the binary via km_locale.qrc and are NOT installed as separate files.
  DESTDIR="$pkgdir" cmake --install build --prefix /usr

  # Ship the project license (matches the sibling `scx-manager` package).
  install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
