// Copyright (C) 2022-2026 Vladislav Nepogodin
//
// This file is part of kernel-manager.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// K22: unit tests for src/aur_kernel.cpp — the AUR build-tree path
// derivation (detail::aur_pkgbuilds_path) and its interaction with the
// user-selectable build directory (utils::build_repo_path /
// utils::set_build_dir). The AUR path is the single derivation point for
// where kernel AUR package build trees live; a regression here would
// silently send makepkg into the wrong directory (or a stale one).
//
// Coverage:
//   A. default build dir: with no buildDir in QSettings,
//      aur_pkgbuilds_path() == ~/.cache/kernel-manager/pkgbuilds/aur_pkgbuilds
//      (the ~ is expanded to $HOME).
//   B. no package name: aur_pkgbuilds_path("") == <buildDir>/aur_pkgbuilds
//      (exactly that, no trailing component).
//   C. with package name: aur_pkgbuilds_path("linux-zen-git") ==
//      <buildDir>/aur_pkgbuilds/linux-zen-git.
//   D. custom build dir: after set_build_dir("/tmp/km-custom"),
//      aur_pkgbuilds_path() == /tmp/km-custom/aur_pkgbuilds (re-read on
//      demand, no cache).
//   E. tilde expansion: set_build_dir("~/km-tilde") expands to
//      $HOME/km-tilde in aur_pkgbuilds_path().
//   F. trailing-separator stripping: set_build_dir("/tmp/km-slash/")
//      stores the stripped value, so aur_pkgbuilds_path() ==
//      /tmp/km-slash/aur_pkgbuilds (not /tmp/km-slash//aur_pkgbuilds).
//   G. empty set_build_dir: a no-op — the current/default dir is kept
//      (aur_pkgbuilds_path() unchanged).
//
// Recipe: standalone g++ against the real aur_kernel.cpp + utils.cpp
// (run_k7.sh precedent), linking fmt + glib + Qt6Core + libalpm. The
// harness sets XDG_CONFIG_HOME to a fresh mktemp sandbox for the whole
// run so the QSettings file (ArchLinux/KernelManager.*) is isolated from
// the user's real config; the sandbox is removed by the trap below on
// every exit path. The test's main() re-pins the variable via setenv
// before any utils:: call, so the binary is also safe to run standalone
// (the k15 D4 seam).

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unistd.h>

#include <fmt/core.h>

#include "aur_kernel.hpp"
#include "utils.hpp"

namespace fs = std::filesystem;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (cond) {                                                          \
            g_pass++;                                                        \
        } else {                                                             \
            g_fail++;                                                        \
            fmt::print(stderr, "K22 FAIL [{}:{}] {}\n", __FILE__, __LINE__,  \
                       msg);                                                \
        }                                                                    \
    } while (0)

int main() {
    // Isolate QSettings in a mktemp sandbox (the k15 D4 seam): the
    // harness exports XDG_CONFIG_HOME; this test re-pins it so the
    // binary is safe to run standalone.
    const char* sandbox_env = std::getenv("XDG_CONFIG_HOME");
    fs::path sandbox;
    if (sandbox_env && !std::string{sandbox_env}.empty()) {
        sandbox = fs::path{sandbox_env};
    } else {
        sandbox = fs::temp_directory_path() / ("km-test-k22-" + std::to_string(::getpid()));
        fs::create_directories(sandbox);
        ::setenv("XDG_CONFIG_HOME", sandbox.string().c_str(), 1);
    }

    // Also isolate the cache dir (where the default buildDir lives).
    const char* cache_env = std::getenv("XDG_CACHE_HOME");
    fs::path cache_sandbox;
    if (cache_env && !std::string{cache_env}.empty()) {
        cache_sandbox = fs::path{cache_env};
    } else {
        cache_sandbox = fs::temp_directory_path() / ("km-test-k22-cache-" + std::to_string(::getpid()));
        fs::create_directories(cache_sandbox);
        ::setenv("XDG_CACHE_HOME", cache_sandbox.string().c_str(), 1);
    }

    const std::string home = fs::canonical(fs::path{std::getenv("HOME")}).string();

    // -----------------------------------------------------------------
    // A. Default build dir (no buildDir in QSettings)
    // -----------------------------------------------------------------
    {
        // Clear any buildDir that might have been set by a previous test.
        // We do this by not setting it at all — a fresh sandbox has none.
        const auto path = detail::aur_pkgbuilds_path();
        const std::string expected = home + "/.cache/kernel-manager/pkgbuilds/aur_pkgbuilds";
        CHECK(path == expected, "A: default aur_pkgbuilds_path() == " + expected + " (got " + path.string() + ")");
    }

    // -----------------------------------------------------------------
    // B. No package name → exactly <buildDir>/aur_pkgbuilds
    // -----------------------------------------------------------------
    {
        utils::set_build_dir("/tmp/km-test-k22-build");
        const auto path = detail::aur_pkgbuilds_path("");
        const std::string expected = "/tmp/km-test-k22-build/aur_pkgbuilds";
        CHECK(path == expected, "B: aur_pkgbuilds_path(\"\") == " + expected + " (got " + path.string() + ")");
    }

    // -----------------------------------------------------------------
    // C. With package name → <buildDir>/aur_pkgbuilds/<package>
    // -----------------------------------------------------------------
    {
        utils::set_build_dir("/tmp/km-test-k22-build");
        const auto path = detail::aur_pkgbuilds_path("linux-zen-git");
        const std::string expected = "/tmp/km-test-k22-build/aur_pkgbuilds/linux-zen-git";
        CHECK(path == expected, "C: aur_pkgbuilds_path(linux-zen-git) == " + expected + " (got " + path.string() + ")");
    }

    // -----------------------------------------------------------------
    // D. Custom build dir re-read on demand (no cache)
    // -----------------------------------------------------------------
    {
        utils::set_build_dir("/tmp/km-test-k22-build-a");
        auto path_a = detail::aur_pkgbuilds_path();
        CHECK(path_a == "/tmp/km-test-k22-build-a/aur_pkgbuilds",
              "D: after set_build_dir(A), path == A (got " + path_a.string() + ")");

        utils::set_build_dir("/tmp/km-test-k22-build-b");
        auto path_b = detail::aur_pkgbuilds_path();
        CHECK(path_b == "/tmp/km-test-k22-build-b/aur_pkgbuilds",
              "D: after set_build_dir(B), path == B immediately (got " + path_b.string() + ")");
        CHECK(path_a != path_b, "D: A != B (no stale cache)");
    }

    // -----------------------------------------------------------------
    // E. Tilde expansion in build dir
    // -----------------------------------------------------------------
    {
        utils::set_build_dir("~/km-tilde-test");
        const auto path = detail::aur_pkgbuilds_path();
        const std::string expected = home + "/km-tilde-test/aur_pkgbuilds";
        CHECK(path == expected, "E: tilde expansion in aur_pkgbuilds_path (got " + path.string() + ")");
    }

    // -----------------------------------------------------------------
    // F. Trailing-separator stripping
    // -----------------------------------------------------------------
    {
        utils::set_build_dir("/tmp/km-test-k22-slash/");
        const auto path = detail::aur_pkgbuilds_path();
        const std::string expected = "/tmp/km-test-k22-slash/aur_pkgbuilds";
        CHECK(path == expected, "F: trailing slash stripped (got " + path.string() + ")");

        utils::set_build_dir("/tmp/km-test-k22-slash//");
        const auto path2 = detail::aur_pkgbuilds_path();
        CHECK(path2 == expected, "F: double trailing slash stripped (got " + path2.string() + ")");
    }

    // -----------------------------------------------------------------
    // G. Empty set_build_dir is a no-op
    // -----------------------------------------------------------------
    {
        utils::set_build_dir("/tmp/km-test-k22-before");
        auto before = detail::aur_pkgbuilds_path();

        utils::set_build_dir("");  // no-op: current dir kept
        auto after = detail::aur_pkgbuilds_path();

        CHECK(before == after, "G: empty set_build_dir is a no-op (before=" + before.string() + ", after=" + after.string() + ")");
    }

    // -----------------------------------------------------------------
    // Cleanup
    // -----------------------------------------------------------------
    // Remove the sandboxes if we created them (the harness also traps).
    if (fs::exists(sandbox)) {
        fs::remove_all(sandbox);
    }
    if (fs::exists(cache_sandbox)) {
        fs::remove_all(cache_sandbox);
    }

    fmt::print("K22: {} passed, {} failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
