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

// Standalone unit test for the chunk-2 known-kernels table (no CTest infra
// in this project; follows the "standalone g++ on the real sources"
// precedent of tests/run_chunk1.sh). The module is pure C++ (no Qt, no
// external deps), so this compiles it directly with the project's warning
// set and asserts on:
//   - the curated table shape (size, unique names, full fields)
//   - default_source_for: the corrected identity mapping (linux -> linux,
//     NO linux-cachyos override), repo-prefixed lookups, and the
//     unknown-name fallback
//   - the install fields (install_package/install_repo/precompiled_available/
//     buildable) + is_installable (curated true, unknown false)
//   - find_kernel (known entry + nullopt)
//   - description_for (curated entry + synthesized fallback)
//   - known_sources (non-empty, deduped, sorted)
//   - kernel_name_from_raw (repo prefix stripping)

#include "known_kernels.hpp"

#include <algorithm> // for sort, unique
#include <cstdio>    // for printf
#include <ranges>    // for ranges::find, sort
#include <string>    // for string
#include <vector>    // for vector

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

}  // namespace

int main() {
    // ------------------------------------------------------------------
    // 1. The curated table: shape.
    // ------------------------------------------------------------------
    const auto& table = km::known_kernels();
    check(table.size() == 13, "table holds the thirteen known kernels (6 official + 7 cachyos)");
    {
        std::vector<std::string> names{};
        for (const auto& kernel : table) {
            names.push_back(kernel.name);
        }
        std::sort(names.begin(), names.end());
        const auto last = std::unique(names.begin(), names.end());
        check(last == names.end(), "table has no duplicate kernel names");
    }
    for (const auto& kernel : table) {
        check(!kernel.name.empty() && !kernel.display_name.empty() && !kernel.description.empty() && !kernel.default_source.empty(),
            "every entry has name, display_name, description, default_source");
        // Install-field consistency: precompiled_available must match the
        // non-emptiness of install_package, and every curated kernel is
        // buildable with a non-empty install repo.
        check(kernel.precompiled_available == !kernel.install_package.empty(),
            "precompiled_available matches install_package non-emptiness");
        check(kernel.buildable && !kernel.install_repo.empty(),
            "every curated entry is buildable with a non-empty install_repo");
    }

    // ------------------------------------------------------------------
    // 2. find_kernel: known entries (bare and repo-prefixed), nullopt.
    // ------------------------------------------------------------------
    check(km::find_kernel("linux").has_value(), "find_kernel(\"linux\") found");
    check(km::find_kernel("core/linux").has_value(), "find_kernel(\"core/linux\") found (prefix stripped)");
    check(!km::find_kernel("unknown-kernel").has_value(), "find_kernel(\"unknown-kernel\") is nullopt");
    if (const auto found = km::find_kernel("linux"); found.has_value()) {
        check((*found)->name == "linux" && (*found)->default_source == "linux",
            "linux entry maps to its own source (linux, not linux-cachyos)");
    }

    // ------------------------------------------------------------------
    // 3. default_source_for: the corrected identity mapping (linux ->
    //    linux, no override), repo-prefixed lookups, and the unknown-name
    //    fallback.
    // ------------------------------------------------------------------
    check(km::default_source_for("linux") == "linux", "default_source_for(\"linux\") == \"linux\" (corrected identity mapping)");
    check(km::default_source_for("core/linux") == "linux", "default_source_for(\"core/linux\") == \"linux\" (raw name, prefix stripped)");
    check(km::default_source_for("linux-zen") == "linux-zen", "default_source_for(\"linux-zen\") == \"linux-zen\"");
    check(km::default_source_for("aur/linux-zen") == "linux-zen", "default_source_for(\"aur/linux-zen\") == \"linux-zen\" (raw name)");
    check(km::default_source_for("linux-lts") == "linux-lts", "default_source_for(\"linux-lts\") == \"linux-lts\"");
    check(km::default_source_for("linux-cachyos") == "linux-cachyos", "default_source_for(\"linux-cachyos\") == \"linux-cachyos\"");
    check(km::default_source_for("linux-hardened") == "linux-hardened", "default_source_for(\"linux-hardened\") == \"linux-hardened\"");
    check(km::default_source_for("linux-rt") == "linux-rt", "default_source_for(\"linux-rt\") == \"linux-rt\"");
    check(km::default_source_for("unknown-kernel") == "unknown-kernel", "unknown kernel falls back to its own name");
    check(km::default_source_for("aur/linux-rc") == "linux-rc", "unknown raw name falls back to the prefix-stripped name");

    // ------------------------------------------------------------------
    // 4. description_for: curated entries (non-empty) and the synthesized
    //    fallback (with and without category, always non-empty).
    // ------------------------------------------------------------------
    check(!km::description_for("linux").empty(), "description_for(\"linux\") is non-empty (curated)");
    check(!km::description_for("linux-zen").empty(), "description_for(\"linux-zen\") is non-empty (curated)");
    check(!km::description_for("unknown").empty(), "description_for(\"unknown\") is non-empty (synthesized)");
    check(!km::description_for("linux-rc", "release candidate").empty(), "description_for(\"linux-rc\", category) is non-empty (synthesized with category)");
    check(km::description_for("linux") != km::description_for("unknown"), "curated and synthesized descriptions differ");
    check(km::description_for("core/linux") == km::description_for("linux"), "description lookup is prefix-insensitive");

    // ------------------------------------------------------------------
    // 5. known_sources: non-empty, contains the mapped default, deduped,
    //    sorted.
    // ------------------------------------------------------------------
    const auto sources = km::known_sources();
    check(!sources.empty(), "known_sources() is non-empty");
    check(std::ranges::find(sources, "linux") != sources.end(), "known_sources() contains \"linux\" (identity)");
    check(std::ranges::find(sources, "linux-cachyos") != sources.end(), "known_sources() contains \"linux-cachyos\" (its own identity entry)");
    {
        auto sorted = sources;
        std::ranges::sort(sorted);
        check(sources == sorted, "known_sources() is sorted");
        const auto last = std::unique(sorted.begin(), sorted.end());
        check(last == sorted.end(), "known_sources() has no duplicates");
    }

    // ------------------------------------------------------------------
    // 6. kernel_name_from_raw: repo prefix stripping.
    // ------------------------------------------------------------------
    check(km::kernel_name_from_raw("core/linux") == "linux", "kernel_name_from_raw(\"core/linux\") == \"linux\"");
    check(km::kernel_name_from_raw("aur/linux-zen") == "linux-zen", "kernel_name_from_raw(\"aur/linux-zen\") == \"linux-zen\"");
    check(km::kernel_name_from_raw("linux") == "linux", "kernel_name_from_raw(\"linux\") == \"linux\" (no prefix)");
    check(km::kernel_name_from_raw("") == "", "kernel_name_from_raw(\"\") == \"\"");

    // ------------------------------------------------------------------
    // 7. is_installable + install-field spot-checks.
    // ------------------------------------------------------------------
    check(km::is_installable("linux"), "is_installable(\"linux\") is true (core pre-compiled)");
    check(km::is_installable("core/linux"), "is_installable(\"core/linux\") is true (prefix-tolerant)");
    check(km::is_installable("linux-zen"), "is_installable(\"linux-zen\") is true (extra pre-compiled)");
    check(km::is_installable("linux-cachyos"), "is_installable(\"linux-cachyos\") is true (cachyos pre-compiled)");
    check(km::is_installable("linux-lts"), "is_installable(\"linux-lts\") is true (core pre-compiled)");
    check(km::is_installable("linux-hardened"), "is_installable(\"linux-hardened\") is true (extra pre-compiled)");
    check(km::is_installable("linux-rt"), "is_installable(\"linux-rt\") is true (extra pre-compiled)");
    check(!km::is_installable("unknown-kernel"), "is_installable(\"unknown-kernel\") is false (unknown)");
    if (const auto found = km::find_kernel("linux"); found.has_value()) {
        check((*found)->install_package == "linux" && (*found)->install_repo == "core" && (*found)->precompiled_available && (*found)->buildable,
            "linux install fields: package=linux repo=core precompiled=true buildable=true");
    }
    if (const auto found = km::find_kernel("linux-cachyos"); found.has_value()) {
        check((*found)->install_package == "linux-cachyos" && (*found)->install_repo == "cachyos" && (*found)->precompiled_available && (*found)->buildable,
            "linux-cachyos install fields: package=linux-cachyos repo=cachyos precompiled=true buildable=true");
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
