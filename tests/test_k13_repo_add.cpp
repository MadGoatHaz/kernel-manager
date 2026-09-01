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

// Standalone unit test for the C7 repo-add contract (k13 part A; no CTest
// infra in this project — follows the "standalone g++ on the real sources"
// precedent of tests/run_k7.sh). Compiles the real src/alpm_utils.cpp
// (+ src/utils.cpp, whose exec() backs the AUR probe, + src/known_kernels.cpp
// for the curated-table guard) with the project's GCC warning set, links
// libalpm + Qt6Core + fmt, and asserts:
//   - is_repo_enabled on temp confs: an active [section] ⇒ true; an absent
//     section ⇒ false; a commented-out "# [section]" ⇒ false; an
//     options-only file ⇒ false (the mINI comment-skip semantics)
//   - is_repo_enabled live-conf spot-checks (gated like k7): core ⇒ true,
//     cachyos ⇒ false on this machine
//   - add_repo_to_pacman_conf with a fake recording runner: the launch
//     contract ("cachyos" ⇒ exactly one call, cmd == the
//     <KM_HELPER_DIR>/repo_add.sh 'cachyos' shape, escalate == true, rc 0)
//     + all four guard rejections ("" / "aur" / table-absent / metacharacter
//     ⇒ non-zero rc, 0 runner calls) + runner rc propagation (3 ⇒ 3)
#include "alpm_utils.hpp"

#include <cassert>     // for the test-scaffolding sanity asserts
#include <cstdio>      // for printf
#include <filesystem>  // for temp-conf creation + the live-conf gate
#include <fstream>     // for writing the temp confs
#include <string>      // for string
#include <string_view> // for string_view
#include <vector>      // for vector (the FakeRunner call log)

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

// Write a temp conf and return its path (unique per test case so runs never
// collide; the file sits in the system temp dir and is harmless if left
// behind — the test reads it, never the system conf).
std::string make_temp_conf(const char* name, const std::string& content) {
    const std::string path =
        (std::filesystem::temp_directory_path() / (std::string{"km-test-k13-"} + name)).string();
    std::ofstream out(path);
    assert(out);
    out << content;
    assert(out);
    return path;
}

// A recording fake of the utils::RepoCmdRunner contract (the k8
// CommandRunner precedent): logs every call (command + escalate flag) and
// returns a configurable rc. Never touches the system.
struct FakeRunner {
    std::vector<std::pair<std::string, bool>> calls;
    int rc = 0;
    int operator()(std::string_view cmd, bool escalate) {
        calls.emplace_back(std::string(cmd), escalate);
        return rc;
    }
};

// Wrap the fake in a by-reference lambda before handing it to
// add_repo_to_pacman_conf: the RepoCmdRunner parameter is a std::function
// taken BY VALUE, so passing the struct directly would record calls in the
// function's internal copy (the k7 AurProbe reference-capture idiom).
utils::RepoCmdRunner recording(FakeRunner& fake) {
    return [&fake](std::string_view cmd, bool escalate) { return fake(cmd, escalate); };
}

}  // namespace

int main() {
    // ------------------------------------------------------------------
    // 1. is_repo_enabled on temp confs (mINI semantics).
    // ------------------------------------------------------------------
    {
        const std::string conf_core =
            make_temp_conf("core.conf", "[core]\nServer = https://mirror.example/core/$repo/$arch\n");
        check(utils::is_repo_enabled("core", conf_core), "is_repo_enabled: active [core] section → true");
    }
    {
        const std::string conf_extra =
            make_temp_conf("extra.conf", "[extra]\nServer = https://mirror.example/extra/$repo/$arch\n");
        check(!utils::is_repo_enabled("cachyos", conf_extra), "is_repo_enabled: [cachyos] absent (only [extra]) → false");
    }
    {
        const std::string conf_commented = make_temp_conf(
            "commented.conf", "# [cachyos]\n# Server = https://mirror.example/cachyos/$repo/$arch\n");
        check(!utils::is_repo_enabled("cachyos", conf_commented), "is_repo_enabled: commented \"# [cachyos]\" → false");
    }
    {
        const std::string conf_options = make_temp_conf("options.conf", "[options]\nNoUpgrade = usr/\n");
        check(!utils::is_repo_enabled("core", conf_options), "is_repo_enabled: options-only file → false");
    }

    // ------------------------------------------------------------------
    // 2. is_repo_enabled live-conf spot-checks (gated like k7: this
    //    machine's /etc/pacman.conf — read-only, never modified).
    // ------------------------------------------------------------------
    if (std::filesystem::exists("/etc/pacman.conf")) {
        check(utils::is_repo_enabled("core"), "is_repo_enabled (live /etc/pacman.conf): core → true");
        check(!utils::is_repo_enabled("cachyos"), "is_repo_enabled (live /etc/pacman.conf): cachyos → false");
    } else {
        std::printf("SKIP: live /etc/pacman.conf absent (non-Arch machine) — spot-checks not run\n");
    }

    // ------------------------------------------------------------------
    // 3. add_repo_to_pacman_conf: the launch contract (fake recording
    //    runner — no real terminal, no pkexec, no system write).
    // ------------------------------------------------------------------
    {
        FakeRunner fake;
        const int rc = utils::add_repo_to_pacman_conf("cachyos", recording(fake));
        check(rc == 0, "add_repo: valid \"cachyos\" → rc 0");
        check(fake.calls.size() == 1u, "add_repo: valid \"cachyos\" → exactly one runner call");
        if (fake.calls.size() == 1u) {
            check(fake.calls[0].second, "add_repo: the call escalates (escalate == true)");
            check(fake.calls[0].first == "/usr/lib/kernel-manager/repo_add.sh 'cachyos'",
                  "add_repo: cmd == /usr/lib/kernel-manager/repo_add.sh 'cachyos' (the exact C4 launch contract)");
            check(fake.calls[0].first.find("repo_add.sh 'cachyos'") != std::string::npos,
                  "add_repo: cmd contains repo_add.sh 'cachyos'");
        }
    }

    // ------------------------------------------------------------------
    // 4. add_repo_to_pacman_conf: the guard rejections — each returns a
    //    non-zero rc and NEVER calls the runner (0 recorded calls).
    // ------------------------------------------------------------------
    const struct {
        const char* repo;
        const char* label;
    } guards[]{
        {"", "empty name"},
        {"aur", "\"aur\" (the AUR is not a pacman repo)"},
        {"bogus", "\"bogus\" (absent from the known-kernel table)"},
        {"a$(b)", "metacharacter (outside [a-z0-9-])"},
        {"a;b", "semicolon (outside [a-z0-9-])"},
    };
    for (const auto& g : guards) {
        FakeRunner fake;
        const int rc = utils::add_repo_to_pacman_conf(g.repo, recording(fake));
        check(rc != 0, (std::string{"add_repo: guard rejects "} + g.label + " → non-zero rc").c_str());
        check(fake.calls.empty(), (std::string{"add_repo: guard rejects "} + g.label + " → 0 runner calls").c_str());
    }

    // ------------------------------------------------------------------
    // 5. add_repo_to_pacman_conf: runner rc propagation.
    // ------------------------------------------------------------------
    {
        FakeRunner fake;
        fake.rc = 3;
        const int rc = utils::add_repo_to_pacman_conf("cachyos", recording(fake));
        check(rc == 3, "add_repo: the runner's rc (3) propagates");
        check(fake.calls.size() == 1u, "add_repo: rc propagation — exactly one runner call");
    }

    if (g_failures == 0) {
        std::printf("K13-CPP: all assertions passed\n");
    } else {
        std::printf("K13-CPP: %d failure(s)\n", g_failures);
    }
    return g_failures;
}
