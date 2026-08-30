// Copyright (C) 2022-2025 Vladislav Nepogodin
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

#include "alpm_utils.hpp"
#include "ini.hpp"
#include "utils.hpp"  // for utils::exec (AUR probe executor)

namespace utils {

// Sync repo excluded from kernel discovery. Build-configurable via the
// KM_IGNORE_REPO CMake variable (default: empty = no exclusion), so no repo
// name is hard-coded in the source.
#ifndef KM_IGNORE_REPO
#define KM_IGNORE_REPO ""
#endif

alpm_handle_t* parse_alpm(std::string_view root, std::string_view dbpath, alpm_errno_t* err) noexcept {
    // Initialize alpm.
    alpm_handle_t* alpm_handle = alpm_initialize(root.data(), dbpath.data(), err);
    if (alpm_handle == nullptr) {
        return nullptr;
    }

    // Parse pacman config.
    static constexpr auto pacman_conf_path = "/etc/pacman.conf";
    static constexpr auto ignored_repo     = KM_IGNORE_REPO;

    const mINI::INIFile file(pacman_conf_path);
    // next, create a structure that will hold data
    mINI::INIStructure ini;

    // now we can read the file
    file.read(ini);
    for (const auto& it : ini) {
        const auto& section = it.first;
        if (section == ignored_repo || section == "options") {
            continue;
        }
        [[maybe_unused]] auto* db = alpm_register_syncdb(alpm_handle, section.c_str(), ALPM_SIG_USE_DEFAULT);
    }

    return alpm_handle;
}

std::int32_t release_alpm(alpm_handle_t* handle, alpm_errno_t* err) noexcept {
    // read errno before
    if (err != nullptr) {
        *err = alpm_errno(handle);
    }
    // Release libalpm handle
    return alpm_release(handle);
}

bool is_aur_package_available(const ExecFn& exec_fn, std::string_view pkg) noexcept {
    // The AUR can only be queried through paru; without it the package is
    // reported as unavailable (the custom build remains the install path).
    const std::string paru_path = exec_fn("command -v paru");
    if (paru_path.empty() || paru_path == "-1") {
        return false;
    }

    // `paru --aur -Si` prints the package metadata to stdout when the
    // package exists and stays silent on stdout (error on stderr) when it
    // does not. The name is single-quoted: pacman package names contain no
    // shell metacharacters.
    const std::string info = exec_fn("paru --aur -Si '" + std::string{pkg} + "'");
    return !info.empty() && info != "-1";
}

bool is_package_in_sync_db(alpm_handle_t* handle, std::string_view repo, std::string_view pkg) noexcept {
    if (handle == nullptr) {
        return false;
    }
    // Locate the sync DB registered under `repo` (parse_alpm registers
    // every non-ignored /etc/pacman.conf section); a missing section means
    // the repo is not added on this system.
    for (alpm_list_t* i = alpm_get_syncdbs(handle); i != nullptr; i = i->next) {
        auto* db = reinterpret_cast<alpm_db_t*>(i->data);
        const char* db_name = alpm_db_get_name(db);
        if (db_name == nullptr || std::string_view{db_name} != repo) {
            continue;
        }
        return alpm_db_get_pkg(db, pkg.data()) != nullptr;
    }
    return false;
}

bool is_package_available(std::string_view pkg, std::string_view repo) noexcept {
    if (classify_repo(repo) == PackageSource::AUR) {
        // Inject utils::exec (popen-based, "-1" on spawn failure) as the
        // AUR probe executor.
        const ExecFn exec_fn{[](std::string_view command) noexcept { return utils::exec(command); }};
        return is_aur_package_available(exec_fn, pkg);
    }

    // Fresh handle per call: read-only sync-DB access, no root, safe to
    // invoke repeatedly (released on the way out).
    alpm_errno_t err{};
    alpm_handle_t* handle = parse_alpm(alpm_root, alpm_libdir, &err);
    if (handle == nullptr) {
        return false;
    }
    const bool available = is_package_in_sync_db(handle, repo, pkg);
    release_alpm(handle, &err);
    return available;
}

}  // namespace utils
