// Copyright (C) 2022-2026 Vladislav Nepogodin
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

#include "aur_kernel.hpp"
#include "utils.hpp"

#include <algorithm>   // for search
#include <filesystem>  // for path
#include <ranges>      // for ranges::*

#include <fmt/format.h>

namespace fs = std::filesystem;

namespace {

void prepare_build_environment(const std::string_view& package_name) noexcept {
    static const fs::path pkgbuilds_path = utils::fix_path("~/.cache/cachyos-km/aur_pkgbuilds");
    const fs::path package_path          = utils::fix_path(fmt::format("~/.cache/cachyos-km/aur_pkgbuilds/{}", package_name));
    utils::prepare_git_repo(pkgbuilds_path, package_path, fmt::format("https://aur.archlinux.org/{}.git", package_name));
}

}  // namespace

namespace detail {

void install_aur_kernels(std::span<std::string> kernel_list) noexcept {
    using namespace std::literals;

    for (auto&& kernel_name : kernel_list) {
        if (auto found = std::ranges::search(kernel_name, "headers"sv); !found.empty()) {
            continue;
        }

        prepare_build_environment(kernel_name);

        // Run our build command!
        utils::runCmdTerminal("makepkg -sicf --cleanbuild --skipchecksums", false);
    }
}

}  // namespace detail
