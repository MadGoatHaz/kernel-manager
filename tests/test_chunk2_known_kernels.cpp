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

// Standalone unit test for the full 21-kernel known-kernels table (K4,
// supersedes the pre-K1 chunk-2 version in place; no CTest infra in this
// project — follows the "standalone g++ on the real sources" precedent of
// tests/run_chunk1.sh). The module is pure C++ (no Qt, no external deps),
// so this compiles it directly with the project's warning set and asserts:
//   - the full table: count == 21, all 21 expected names present, names
//     unique, every entry's name/display_name/description/default_source
//     non-empty (description checked per entry)
//   - install-field consistency: precompiled_available == (install_package
//     non-empty) for every entry; install_repo within the allowed set;
//     every entry buildable; per-kernel install data spot-checks
//   - the corrected mapping: default_source_for identity for every entry
//     (linux -> linux, linux-zen -> linux-zen, ...; linux-tkg -> its
//     github URL), repo-prefix-tolerant lookups, unknown-name fallback
//   - is_installable: curated true (prefix-tolerant), linux-tkg false
//     (no pre-compiled package), unknown false; consistent with the table
//   - find_kernel: known + repo-prefixed + nullopt for unknown
//   - description_for: curated non-empty, synthesized fallback (with and
//     without category) non-empty, prefix-insensitive
//   - known_sources: non-empty, contains "linux" + the tkg git URL,
//     deduped and sorted
//   - kernel_name_from_raw: repo prefix stripping

#include "known_kernels.hpp"

#include <algorithm>   // for sort, unique
#include <cstdio>      // for printf
#include <ranges>      // for ranges::find, sort
#include <string>      // for string
#include <string_view> // for string_view
#include <vector>      // for vector

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    if (condition) {
        std::printf("PASS: %s\n", what);
    } else {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// Per-kernel variant with a dynamic name in the PASS/FAIL line so a
// failure identifies exactly which entry tripped.
void check_named(bool condition, const char* verb, std::string_view kernel) {
    ++g_checks;
    const std::string name{kernel};
    if (condition) {
        std::printf("PASS: %s %s\n", verb, name.c_str());
    } else {
        ++g_failures;
        std::printf("FAIL: %s %s\n", verb, name.c_str());
    }
}

}  // namespace

int main() {
    const auto& table = km::known_kernels();

    // ------------------------------------------------------------------
    // 1. The full table: count, expected names, uniqueness, field shape.
    // ------------------------------------------------------------------
    check(table.size() == 21, "table holds the twenty-one known kernels (6 official + 7 cachyos + 8 community)");

    std::vector<std::string> names{};
    names.reserve(table.size());
    for (const auto& kernel : table) {
        names.push_back(kernel.name);
    }

    {
        auto sorted_names = names;
        std::ranges::sort(sorted_names);
        const auto last = std::unique(sorted_names.begin(), sorted_names.end());
        check(last == sorted_names.end(), "table has no duplicate kernel names");
    }

    // All 21 expected names present (one check per name so a failure names
    // the missing kernel).
    constexpr std::string_view kExpectedNames[21] = {
        "linux", "linux-lts", "linux-zen", "linux-hardened", "linux-rt", "linux-rt-lts",
        "linux-mainline", "linux-cachyos", "linux-cachyos-bore", "linux-cachyos-rt-bore",
        "linux-cachyos-lts", "linux-cachyos-server", "linux-cachyos-deckify", "linux-cachyos-bmq",
        "linux-tkg", "linux-xanmod", "linux-xanmod-edge", "linux-xanmod-lts", "linux-xanmod-rt",
        "linux-lqx", "linux-clear",
    };
    for (const auto name : kExpectedNames) {
        check_named(std::ranges::find(names, name) != names.end(), "expected name present:", name);
    }

    // Every entry: core fields non-empty; description non-empty (per entry).
    {
        bool all_fields_nonempty = true;
        for (const auto& kernel : table) {
            if (kernel.name.empty() || kernel.display_name.empty() || kernel.default_source.empty()) {
                all_fields_nonempty = false;
                std::printf("  empty core field in: %s\n", kernel.name.c_str());
            }
        }
        check(all_fields_nonempty, "every entry has non-empty name, display_name, default_source");
    }
    for (const auto& kernel : table) {
        check_named(!kernel.description.empty(), "description non-empty:", kernel.name);
    }

    // ------------------------------------------------------------------
    // 2. Install-field consistency + per-kernel data spot-checks.
    // ------------------------------------------------------------------
    {
        bool consistent = true;
        for (const auto& kernel : table) {
            if (kernel.precompiled_available != !kernel.install_package.empty()) {
                consistent = false;
                std::printf("  inconsistent: %s (precompiled_available=%d, install_package='%s')\n",
                           kernel.name.c_str(),
                           kernel.precompiled_available ? 1 : 0,
                           kernel.install_package.c_str());
            }
        }
        check(consistent, "precompiled_available == (install_package non-empty) for every entry");
    }
    {
        const std::vector<std::string_view> allowed_repos{"core", "extra", "cachyos", "chaotic-aur", "aur", "liquorix"};
        bool repos_ok = true;
        for (const auto& kernel : table) {
            if (!kernel.install_repo.empty() && std::ranges::find(allowed_repos, kernel.install_repo) == allowed_repos.end()) {
                repos_ok = false;
                std::printf("  unexpected install_repo: %s -> %s\n", kernel.name.c_str(), kernel.install_repo.c_str());
            }
        }
        check(repos_ok, "every install_repo is one of core|extra|cachyos|chaotic-aur|aur|liquorix");
    }
    {
        bool all_buildable = true;
        for (const auto& kernel : table) {
            if (!kernel.buildable) {
                all_buildable = false;
                std::printf("  not buildable: %s\n", kernel.name.c_str());
            }
        }
        check(all_buildable, "every curated entry is buildable");
    }
    if (const auto entry = km::find_kernel("linux")) {
        check((*entry)->default_source == "linux" && (*entry)->install_package == "linux" && (*entry)->install_repo == "core" &&
                  (*entry)->precompiled_available && (*entry)->buildable,
              "linux data: source=linux package=linux repo=core precompiled buildable");
    } else {
        check(false, "linux entry found for data check");
    }
    if (const auto entry = km::find_kernel("linux-tkg")) {
        check((*entry)->default_source == "https://github.com/Frogging-Family/linux-tkg.git" && (*entry)->install_package.empty() &&
                  !(*entry)->precompiled_available && (*entry)->install_repo == "chaotic-aur" && (*entry)->buildable,
              "linux-tkg data: source=tkg git URL package='' !precompiled repo=chaotic-aur buildable");
    } else {
        check(false, "linux-tkg entry found for data check");
    }
    if (const auto entry = km::find_kernel("linux-lqx")) {
        check((*entry)->install_package == "linux-lqx" && (*entry)->install_repo == "liquorix" && (*entry)->precompiled_available,
              "linux-lqx data: package=linux-lqx repo=liquorix precompiled");
    } else {
        check(false, "linux-lqx entry found for data check");
    }
    if (const auto entry = km::find_kernel("linux-mainline")) {
        check((*entry)->install_package == "linux-mainline" && (*entry)->install_repo == "chaotic-aur" && (*entry)->precompiled_available,
              "linux-mainline data: package=linux-mainline repo=chaotic-aur precompiled");
    } else {
        check(false, "linux-mainline entry found for data check");
    }
    if (const auto entry = km::find_kernel("linux-cachyos")) {
        check((*entry)->install_package == "linux-cachyos" && (*entry)->install_repo == "cachyos" && (*entry)->precompiled_available,
              "linux-cachyos data: package=linux-cachyos repo=cachyos precompiled");
    } else {
        check(false, "linux-cachyos entry found for data check");
    }
    if (const auto entry = km::find_kernel("linux-xanmod")) {
        check((*entry)->install_package == "linux-xanmod" && (*entry)->install_repo == "chaotic-aur" && (*entry)->precompiled_available,
              "linux-xanmod data: package=linux-xanmod repo=chaotic-aur precompiled");
    } else {
        check(false, "linux-xanmod entry found for data check");
    }

    // ------------------------------------------------------------------
    // 3. default_source_for: corrected identity mapping for every entry
    //    (linux -> linux, NOT linux-cachyos; linux-tkg -> its github URL),
    //    repo-prefix-tolerant lookups, and the unknown-name fallback.
    // ------------------------------------------------------------------
    for (const auto& kernel : table) {
        const auto expected = (kernel.name == "linux-tkg")
                                  ? std::string_view{"https://github.com/Frogging-Family/linux-tkg.git"}
                                  : std::string_view{kernel.name};
        check_named(km::default_source_for(kernel.name) == expected, "default_source_for identity mapping:", kernel.name);
    }
    check(km::default_source_for("linux") == "linux", "default_source_for(\"linux\") == \"linux\" (corrected identity, NOT linux-cachyos)");
    check(km::default_source_for("linux-zen") == "linux-zen", "default_source_for(\"linux-zen\") == \"linux-zen\"");
    check(km::default_source_for("core/linux") == "linux", "default_source_for(\"core/linux\") == \"linux\" (prefix-tolerant)");
    check(km::default_source_for("aur/linux-zen") == "linux-zen", "default_source_for(\"aur/linux-zen\") == \"linux-zen\" (prefix-tolerant)");
    check(km::default_source_for("totally-unknown-kernel") == "totally-unknown-kernel", "unknown kernel falls back to its own name");
    check(km::default_source_for("aur/totally-unknown-kernel") == "totally-unknown-kernel", "unknown raw kernel falls back to the prefix-stripped name");

    // ------------------------------------------------------------------
    // 4. is_installable: curated true (prefix-tolerant), linux-tkg false
    //    (no pre-compiled package — build only), unknown false, and
    //    consistent with the table's precompiled flags for every entry.
    // ------------------------------------------------------------------
    check(km::is_installable("linux"), "is_installable(\"linux\") is true (core pre-compiled)");
    check(km::is_installable("core/linux"), "is_installable(\"core/linux\") is true (prefix-tolerant)");
    check(km::is_installable("linux-zen"), "is_installable(\"linux-zen\") is true (extra pre-compiled)");
    check(km::is_installable("linux-cachyos"), "is_installable(\"linux-cachyos\") is true (cachyos pre-compiled)");
    check(!km::is_installable("linux-tkg"), "is_installable(\"linux-tkg\") is false (no pre-compiled package, build only)");
    check(!km::is_installable("totally-unknown-kernel"), "is_installable(\"totally-unknown-kernel\") is false (unknown)");
    for (const auto& kernel : table) {
        check_named(km::is_installable(kernel.name) == (kernel.precompiled_available && !kernel.install_package.empty()),
                    "is_installable matches table flags:", kernel.name);
    }

    // ------------------------------------------------------------------
    // 5. find_kernel: known (bare + repo-prefixed), all 21, nullopt.
    // ------------------------------------------------------------------
    check(km::find_kernel("linux").has_value(), "find_kernel(\"linux\") found");
    check(km::find_kernel("core/linux").has_value(), "find_kernel(\"core/linux\") found (prefix stripped)");
    check(!km::find_kernel("totally-unknown-kernel").has_value(), "find_kernel(\"totally-unknown-kernel\") is nullopt");
    {
        bool all_found = true;
        for (const auto& kernel : table) {
            if (!km::find_kernel(kernel.name).has_value()) {
                all_found = false;
                std::printf("  not found: %s\n", kernel.name.c_str());
            }
        }
        check(all_found, "find_kernel() resolves every curated table name");
    }

    // ------------------------------------------------------------------
    // 6. description_for: curated non-empty, synthesized fallback (with
    //    and without category) always non-empty, prefix-insensitive.
    // ------------------------------------------------------------------
    check(!km::description_for("linux").empty(), "description_for(\"linux\") is non-empty (curated)");
    check(!km::description_for("linux-zen").empty(), "description_for(\"linux-zen\") is non-empty (curated)");
    check(!km::description_for("totally-unknown-kernel").empty(), "description_for(\"totally-unknown-kernel\") is non-empty (synthesized)");
    check(!km::description_for("totally-unknown-kernel", "release candidate").empty(), "description_for(unknown, category) is non-empty (synthesized with category)");
    check(km::description_for("linux") != km::description_for("totally-unknown-kernel"), "curated and synthesized descriptions differ");
    check(km::description_for("core/linux") == km::description_for("linux"), "description lookup is prefix-insensitive");

    // ------------------------------------------------------------------
    // 7. known_sources: non-empty, contains "linux" + the tkg git URL,
    //    deduped, sorted.
    // ------------------------------------------------------------------
    const auto sources = km::known_sources();
    check(!sources.empty(), "known_sources() is non-empty");
    check(std::ranges::find(sources, "linux") != sources.end(), "known_sources() contains \"linux\" (identity)");
    check(std::ranges::find(sources, "linux-cachyos") != sources.end(), "known_sources() contains \"linux-cachyos\" (its own identity entry)");
    check(std::ranges::find(sources, "https://github.com/Frogging-Family/linux-tkg.git") != sources.end(), "known_sources() contains the linux-tkg git URL");
    {
        auto sorted = sources;
        std::ranges::sort(sorted);
        check(sources == sorted, "known_sources() is sorted");
        const auto last = std::unique(sorted.begin(), sorted.end());
        check(last == sorted.end(), "known_sources() has no duplicates");
    }

    // ------------------------------------------------------------------
    // 8. kernel_name_from_raw: repo prefix stripping.
    // ------------------------------------------------------------------
    check(km::kernel_name_from_raw("core/linux") == "linux", "kernel_name_from_raw(\"core/linux\") == \"linux\"");
    check(km::kernel_name_from_raw("aur/linux-zen") == "linux-zen", "kernel_name_from_raw(\"aur/linux-zen\") == \"linux-zen\"");
    check(km::kernel_name_from_raw("linux") == "linux", "kernel_name_from_raw(\"linux\") == \"linux\" (no prefix)");
    check(km::kernel_name_from_raw("") == "", "kernel_name_from_raw(\"\") == \"\"");
    check(km::kernel_name_from_raw("totally-unknown-kernel") == "totally-unknown-kernel", "kernel_name_from_raw leaves bare names untouched");

    // ------------------------------------------------------------------
    // Summary.
    // ------------------------------------------------------------------
    std::printf("%d/%d assertions passed\n", g_checks - g_failures, g_checks);
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
