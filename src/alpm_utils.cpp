// Copyright (C) 2022-2025 Vladislav Nepogodin
//
// This file is part of CachyOS kernel manager.
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

namespace utils {

alpm_handle_t* parse_alpm(std::string_view root, std::string_view dbpath, alpm_errno_t* err) noexcept {
    // Initialize alpm.
    alpm_handle_t* alpm_handle = alpm_initialize(root.data(), dbpath.data(), err);
    if (alpm_handle == nullptr) {
        return nullptr;
    }

    // Parse pacman config.
    static constexpr auto pacman_conf_path = "/etc/pacman.conf";
    static constexpr auto ignored_repo     = "testing";

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

}  // namespace utils
