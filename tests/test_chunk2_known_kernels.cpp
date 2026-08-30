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
//   - default_source_for incl. the user-specified linux -> linux-cachyos
//     override, repo-prefixed lookups, and the unknown-name fallback
//   - find_kernel (known entry + nullopt)
//   - description_for (curated entry + synthesized fallback)
//   - known_sources (non-empty, contains the mapped default, deduped,
//     sorted)
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
    check(table.size() == 6, "table holds the six known kernels");
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
    }

    // ------------------------------------------------------------------
    // 2. find_kernel: known entries (bare and repo-prefixed), nullopt.
    // ------------------------------------------------------------------
    check(km::find_kernel("linux").has_value(), "find_kernel(\"linux\") found");
    check(km::find_kernel("core/linux").has_value(), "find_kernel(\"core/linux\") found (prefix stripped)");
    check(!km::find_kernel("unknown-kernel").has_value(), "find_kernel(\"unknown-kernel\") is nullopt");
    if (const auto found = km::find_kernel("linux"); found.has_value()) {
        check((*found)->name == "linux" && (*found)->default_source == "linux-cachyos", "linux entry maps to linux-cachyos");
    }

    // ------------------------------------------------------------------
    // 3. default_source_for: the user-specified override, the identity
    //    entries, repo-prefixed lookups, and the unknown-name fallback.
    // ------------------------------------------------------------------
    check(km::default_source_for("linux") == "linux-cachyos", "default_source_for(\"linux\") == \"linux-cachyos\" (user-specified override)");
    check(km::default_source_for("core/linux") == "linux-cachyos", "default_source_for(\"core/linux\") == \"linux-cachyos\" (raw name)");
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
    check(std::ranges::find(sources, "linux-cachyos") != sources.end(), "known_sources() contains \"linux-cachyos\"");
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

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
