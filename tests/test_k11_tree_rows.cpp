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

// Standalone unit test for the Kernel::get_kernels curated info-rows (E17 /
// "k11"; no CTest infra in this project — follows the "standalone g++ on the
// real sources" precedent of tests/run_k7.sh). Compiles the real kernel.cpp
// (plus known_kernels.cpp, alpm_utils.cpp and utils.cpp, whose parse_alpm /
// exec back the DB probes) with the project's GCC warning set and asserts:
//   (1) every row's has_pkg() is consistent with its origin — a row is live
//       iff its (repo, name) is a package present in a registered sync DB of
//       the handle (AUR rows are pkg-less by design; the AUR pass is
//       compiled out in this build, so no AUR rows exist here)
//   (2) the info-rows are EXACTLY (curated precompiled_available names)
//       minus (names already listed by the repo/AUR pass); each info-row has
//       has_pkg() == false, version() == "—", get_raw() == "<install_repo>/<name>"
//       and get_repo() == the curated entry's install_repo
//   (3) no duplicate m_name in the full list
//   (4) is_installed() on an info-row equals a direct
//       alpm_db_get_pkg(localdb, name) != nullptr probe (the null-pkg path
//       is crash-free and truthful)
//   (5) install() / remove() on an info-row return false and leave
//       get_install_list() / get_removal_list() untouched
// Environment-tolerant per the k7 precedent: every expected set is computed
// from the live local/sync DBs and the curated table — no hard-coded row
// counts. On a stock checkout of this machine the split reports as
// 6 live + 14 info rows (the plan's "15" counted build-only linux-tkg,
// which has no precompiled row by design).
#include "kernel.hpp"
#include "known_kernels.hpp"
#include "utils.hpp"  // for parse_alpm, release_alpm, alpm_root, alpm_libdir

#include <alpm.h>     // for alpm_*
#include <cstdio>     // for printf
#include <cstdlib>    // for to_string
#include <set>        // for set
#include <string>     // for string, string_view
#include <vector>     // for vector

namespace {

int g_failures = 0;
std::size_t g_checks = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    if (condition) {
        std::printf("PASS: %s\n", what);
    } else {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// The row's package name: the "repo/" prefix of m_raw stripped
// (kernel_name_from_raw yields a view into m_raw; copied out for set storage).
std::string name_of(const Kernel& k) {
    return std::string{km::kernel_name_from_raw(k.get_raw())};
}

// A row's origin is "live" iff its repo is a registered sync DB of the
// handle and the named package is present in it. An AUR row (repo "aur")
// is not sync-DB-sourced; a "local/<name>" row (the D3 pass) is sourced
// from the local DB, so it is live iff the package is present there; an
// info-row of a disabled (or package-less) repo matches nothing.
bool origin_is_live(alpm_handle_t* handle, const Kernel& k) {
    const std::string_view repo = k.get_repo();
    if (repo == "aur") {
        return false;
    }
    const std::string name = name_of(k);
    if (repo == "local") {
        // D-G: the D3 pass emits a local/<name> row only when the package is
        // in the local DB (non-null m_pkg), so such a row is "live" iff the
        // package is present in the local DB. Probe the real local-DB state
        // here (never assume a fixed row set) so the check stays robust to
        // machine-specific local-DB contents — the prior
        // local/linux-cachyos-custom failure class.
        auto* localdb = alpm_get_localdb(handle);
        return localdb != nullptr && alpm_db_get_pkg(localdb, name.c_str()) != nullptr;
    }
    for (alpm_list_t* i = alpm_get_syncdbs(handle); i != nullptr; i = i->next) {
        auto* db = reinterpret_cast<alpm_db_t*>(i->data);
        if (std::string_view{alpm_db_get_name(db)} != repo) {
            continue;
        }
        return alpm_db_get_pkg(db, name.c_str()) != nullptr;
    }
    return false;
}

}  // namespace

int main() {
    alpm_errno_t err{};
    auto* handle = utils::parse_alpm(utils::alpm_root, utils::alpm_libdir, &err);
    check(handle != nullptr, "parse_alpm handle created (read-only, no root)");
    if (handle == nullptr) {
        std::printf("K11: FATAL — no alpm handle, cannot probe the DBs\n");
        std::printf("K11: %zu checks, %d failures\n", g_checks, g_failures);
        return 2;
    }

    auto kernels = Kernel::get_kernels(handle);

    std::vector<Kernel*> live{};
    std::vector<Kernel*> info{};
    std::vector<Kernel*> aur{};
    for (auto& k : kernels) {
        if (k.has_pkg()) {
            live.push_back(&k);
        } else if (k.get_repo() == "aur") {
            aur.push_back(&k);
        } else {
            info.push_back(&k);
        }
    }

    std::printf("INFO: rows=%zu total = %zu live + %zu info (+%zu aur; the AUR pass is compiled out)\n", kernels.size(), live.size(), info.size(), aur.size());
    for (auto* k : live) {
        std::printf("INFO:   live: %s\n", k->get_raw());
    }
    for (auto* k : info) {
        std::printf("INFO:   info: %s\n", k->get_raw());
    }

    // ------------------------------------------------------------------
    // (1) has_pkg() consistent with the row's origin (repo/AUR rows ⇒ true).
    // ------------------------------------------------------------------
    for (auto& k : kernels) {
        const bool live_origin = origin_is_live(handle, k);
        check(k.has_pkg() == live_origin,
              (std::string{"origin: has_pkg()="} + (k.has_pkg() ? "true" : "false") + ", sync-DB origin=" + (live_origin ? "true" : "false") + " for raw " + k.get_raw()).c_str());
    }

    // ------------------------------------------------------------------
    // (2) info-rows == curated precompiled_available − names already listed.
    // ------------------------------------------------------------------
    std::set<std::string> curated_precompiled{};
    for (const auto& e : km::known_kernels()) {
        if (e.precompiled_available) {
            curated_precompiled.insert(e.name);
        }
    }
    std::set<std::string> pre_listed{};
    for (auto* k : live) {
        pre_listed.insert(name_of(*k));
    }
    for (auto* k : aur) {
        pre_listed.insert(name_of(*k));
    }
    std::set<std::string> expected_info{};
    for (const auto& n : curated_precompiled) {
        if (pre_listed.find(n) == pre_listed.end()) {
            expected_info.insert(n);
        }
    }
    std::set<std::string> actual_info{};
    for (auto* k : info) {
        actual_info.insert(name_of(*k));
    }
    std::printf("INFO: curated precompiled=%zu, already-listed=%zu → expected info=%zu, actual info=%zu\n", curated_precompiled.size(), pre_listed.size(), expected_info.size(), actual_info.size());
    check(expected_info == actual_info, "info-rows are EXACTLY (curated precompiled_available − names already listed)");
    for (auto* k : info) {
        const std::string name = name_of(*k);
        check(!k->has_pkg(), ("info-row: has_pkg() false for " + name).c_str());
        check(k->version() == "—", ("info-row: version() \"—\" for " + name).c_str());
        if (const auto opt = km::find_kernel(name)) {
            const auto* e = *opt;
            check(k->get_repo() == e->install_repo, ("info-row: repo == curated install_repo for " + name).c_str());
            check(std::string{k->get_raw()} == e->install_repo + "/" + name, ("info-row: raw == \"<install_repo>/<name>\" for " + name).c_str());
        } else {
            check(false, ("info-row: curated table entry missing for " + name).c_str());
        }
    }

    // ------------------------------------------------------------------
    // (3) no duplicate m_name in the full list.
    // ------------------------------------------------------------------
    std::set<std::string> all_names{};
    for (const auto& k : kernels) {
        all_names.insert(name_of(k));
    }
    check(all_names.size() == kernels.size(),
          (std::string{"no duplicate m_name in the full list ("} + std::to_string(kernels.size()) + " rows, " + std::to_string(all_names.size()) + " distinct names)").c_str());

    // ------------------------------------------------------------------
    // (4) is_installed() on an info-row == direct local-DB probe.
    // ------------------------------------------------------------------
    auto* localdb = alpm_get_localdb(handle);
    check(localdb != nullptr, "local DB present (alpm_get_localdb)");
    if (localdb != nullptr) {
        for (auto* k : info) {
            const std::string name = name_of(*k);
            const bool direct = alpm_db_get_pkg(localdb, name.c_str()) != nullptr;
            check(k->is_installed() == direct,
                  ("info-row: is_installed() agrees with the direct local-DB probe for " + name + (direct ? " (installed)" : " (not installed)")).c_str());
        }
    }

    // ------------------------------------------------------------------
    // (5) install() / remove() on an info-row: false, globals untouched.
    // ------------------------------------------------------------------
    auto& install_list = Kernel::get_install_list();
    auto& removal_list = Kernel::get_removal_list();
    const auto install_before = install_list;
    const auto removal_before = removal_list;
    for (auto* k : info) {
        const std::string name = name_of(*k);
        check(!k->install(), ("info-row: install() false for " + name).c_str());
        check(!k->remove(), ("info-row: remove() false for " + name).c_str());
    }
    check(install_list == install_before, "get_install_list() untouched by info-row install() calls");
    check(removal_list == removal_before, "get_removal_list() untouched by info-row remove() calls");

    // ------------------------------------------------------------------
    // Build-only curated entries (linux-tkg) must stay out of the tree.
    // ------------------------------------------------------------------
    if (const auto tkg = km::find_kernel("linux-tkg")) {
        check(!(*tkg)->precompiled_available, "curated table: linux-tkg is build-only (no precompiled package)");
    }
    bool tkg_row = false;
    for (const auto& k : kernels) {
        tkg_row = tkg_row || (name_of(k) == "linux-tkg");
    }
    check(!tkg_row, "linux-tkg absent from the tree (build-only)");

    utils::release_alpm(handle, &err);

    std::printf("K11: %zu checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("K11: all assertions passed\n");
    }
    return g_failures;
}
