// Copyright (C) 2026 Vladislav Nepogodin
//
// This file is part of kernel-manager.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

// Standalone unit test for the K7 package-availability helper (no CTest
// infra in this project; follows the "standalone g++ on the real sources"
// precedent of tests/run_chunk1.sh). Compiles the real src/alpm_utils.cpp
// (plus src/utils.cpp, whose exec() backs the AUR probe) with the project's
// GCC warning set and asserts:
//   - classify_repo: "aur" → AUR; pacman repo names (official + third-party
//     + empty) → PACMAN_REPO
//   - is_aur_package_available with an injected fake exec: paru present +
//     info output → true; paru present but silent/no output → false; paru
//     missing → false (graceful, no info probe issued); spawn failure
//     ("-1") → false; the info probe command is `paru --aur -Si '<pkg>'`
//   - is_package_in_sync_db against a real parse_alpm handle: unknown repo
//     → false; null handle → false; "linux" in core → true iff the local
//     core sync DB exists on this machine, otherwise false (environment-
//     tolerant: a missing DB never hard-fails the test)
//   - is_package_available end-to-end (no root): AUR path graceful without
//     paru, repo path matches the sync-DB truth, repeated calls stable
#include "alpm_utils.hpp"
#include "utils.hpp"

#include <cstdio>      // for printf
#include <filesystem>  // for exists
#include <string>      // for string

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::printf("PASS: %s\n", what);
    } else {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// Injectable fake of the AUR probe executor: answers `command -v paru` with
// a configurable path and records every info probe issued.
struct AurProbe {
    std::string paru_path{"/usr/bin/paru"};  // "" = paru not installed
    std::string info{};                      // stdout of the info probe
    int info_calls{};
    std::string last_info_cmd{};
    utils::ExecFn exec_fn{[&](std::string_view command) noexcept -> std::string {
        if (command == "command -v paru") {
            return paru_path;
        }
        last_info_cmd = command;
        ++info_calls;
        return info;
    }};
};

}  // namespace

int main() {
    // ------------------------------------------------------------------
    // 1. classify_repo (pure).
    // ------------------------------------------------------------------
    check(utils::classify_repo("aur") == utils::PackageSource::AUR, "classify_repo(\"aur\") is the AUR");
    for (const auto* repo : {"core", "extra", "community", "multilib", "cachyos", "chaotic-aur", "liquorix", "endeavouros", ""}) {
        check(utils::classify_repo(repo) == utils::PackageSource::PACMAN_REPO,
            (std::string{"classify_repo(\""} + repo + "\") is a pacman repo").c_str());
    }

    // ------------------------------------------------------------------
    // 2. is_aur_package_available with the injected fake exec.
    // ------------------------------------------------------------------
    {
        AurProbe probe{};  // paru present, silent info probe
        check(!utils::is_aur_package_available(probe.exec_fn, "linux-xanmod"), "AUR: paru present, no info output → false");
        check(probe.info_calls == 1, "AUR: info probe issued exactly once");
        check(probe.last_info_cmd == "paru --aur -Si 'linux-xanmod'", "AUR: info probe is `paru --aur -Si '<pkg>'`");
    }
    {
        AurProbe probe{};
        probe.info = "repo    : AUR\nname      : linux-xanmod\nversion   : 6.15.4-1-xanmod1";
        check(utils::is_aur_package_available(probe.exec_fn, "linux-xanmod"), "AUR: info output present → true");
    }
    {
        AurProbe probe{};
        probe.paru_path = "";  // paru not installed
        check(!utils::is_aur_package_available(probe.exec_fn, "linux-xanmod"), "AUR: paru missing → false (graceful)");
        check(probe.info_calls == 0, "AUR: no info probe issued when paru is missing");
    }
    {
        AurProbe probe{};
        probe.info = "-1";  // popen spawn failure
        check(!utils::is_aur_package_available(probe.exec_fn, "linux-xanmod"), "AUR: probe spawn failure (\"-1\") → false");
    }

    // ------------------------------------------------------------------
    // 3. is_package_in_sync_db against a real parse_alpm handle.
    // ------------------------------------------------------------------
    alpm_errno_t err{};
    auto* handle = utils::parse_alpm(utils::alpm_root, utils::alpm_libdir, &err);
    check(handle != nullptr, "parse_alpm handle created (read-only, no root)");
    check(!utils::is_package_in_sync_db(nullptr, "core", "linux"), "sync db: null handle → false");
    if (handle != nullptr) {
        check(!utils::is_package_in_sync_db(handle, "no-such-repo-k7", "linux"), "sync db: unknown repo → false");
        // Environment-tolerant: "linux" in core is asserted true only when
        // the local core sync DB exists on this machine; a missing DB must
        // yield false gracefully, never a test hard-fail.
        const bool core_db_present = std::filesystem::exists("/var/lib/pacman/sync/core.db");
        if (core_db_present) {
            check(utils::is_package_in_sync_db(handle, "core", "linux"), "sync db: core/linux present → true");
            check(!utils::is_package_in_sync_db(handle, "core", "k7-no-such-package-xyz"), "sync db: core/<absent pkg> → false");
        } else {
            check(!utils::is_package_in_sync_db(handle, "core", "linux"), "sync db: core DB missing → false (graceful)");
        }
        utils::release_alpm(handle, &err);
    }

    // ------------------------------------------------------------------
    // 4. is_package_available end-to-end (no root, repeat-safe).
    // ------------------------------------------------------------------
    if (utils::exec("command -v paru").empty()) {
        check(!utils::is_package_available("linux-xanmod", "aur"), "available: AUR without paru → false (graceful)");
    } else {
        // paru present: the public wrapper must agree with the AUR probe
        // driven by the real executor (one read-only AUR query).
        const utils::ExecFn real_exec{[](std::string_view command) noexcept { return utils::exec(command); }};
        check(utils::is_package_available("linux-xanmod", "aur") == utils::is_aur_package_available(real_exec, "linux-xanmod"),
            "available: AUR path agrees with the real-exec probe (no crash)");
    }
    if (std::filesystem::exists("/var/lib/pacman/sync/core.db")) {
        check(utils::is_package_available("linux", "core"), "available: core/linux (no root) → true");
        check(!utils::is_package_available("k7-no-such-package-xyz", "core"), "available: core/<absent pkg> → false");
    } else {
        check(!utils::is_package_available("linux", "core"), "available: core DB missing → false (graceful)");
    }
    check(!utils::is_package_available("linux", "no-such-repo-k7"), "available: unknown repo → false");
    const bool first = utils::is_package_available("linux", "core");
    const bool second = utils::is_package_available("linux", "core");
    check(first == second, "available: repeated calls are stable");

    if (g_failures == 0) {
        std::printf("K7: all assertions passed\n");
    }
    return g_failures;
}
