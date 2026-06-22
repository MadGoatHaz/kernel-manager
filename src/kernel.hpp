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

#ifndef KERNEL_HPP
#define KERNEL_HPP

#include <algorithm>    // for search
#include <ranges>       // for ranges::*
#include <string>       // for string
#include <string_view>  // for string_view
#include <vector>       // for vector

#include <alpm.h>

class Kernel {
 public:
    constexpr Kernel() = default;
    explicit Kernel(alpm_handle_t* handle, alpm_pkg_t* pkg, alpm_pkg_t* headers) : m_name(alpm_pkg_get_name(pkg)), m_pkg(pkg), m_headers(headers), m_handle(handle) { }
    explicit Kernel(alpm_handle_t* handle, alpm_pkg_t* pkg, alpm_pkg_t* headers, const std::string_view& repo) : m_name(alpm_pkg_get_name(pkg)), m_repo(repo), m_pkg(pkg), m_headers(headers), m_handle(handle) { }
    explicit Kernel(alpm_handle_t* handle, alpm_pkg_t* pkg, alpm_pkg_t* headers, const std::string_view& repo, const std::string_view& raw) : m_name(alpm_pkg_get_name(pkg)), m_repo(repo), m_raw(raw), m_pkg(pkg), m_headers(headers), m_handle(handle) { }

    constexpr std::string_view category() const noexcept {
        using namespace std::string_view_literals;
        constexpr std::string_view lto{"lto"};
        constexpr std::string_view lts{"lts"};
        constexpr std::string_view zen{"zen"};
        constexpr std::string_view hardened{"hardened"};
        constexpr std::string_view deckify{"deckify"};
        constexpr std::string_view server{"server"};
        constexpr std::string_view next{"next"};
        constexpr std::string_view mainline{"mainline"};
        constexpr std::string_view git{"git"};
        constexpr std::string_view rc{"rc"};

        auto found = std::ranges::search(m_name, lto);
        if (!found.empty()) {
            return "lto optimized"sv;
        }
        found = std::ranges::search(m_name, lts);
        if (!found.empty()) {
            return "longterm"sv;
        }
        found = std::ranges::search(m_name, zen);
        if (!found.empty()) {
            return "zen-kernel"sv;
        }
        found = std::ranges::search(m_name, hardened);
        if (!found.empty()) {
            return "hardened kernel"sv;
        }
        found = std::ranges::search(m_name, deckify);
        if (!found.empty()) {
            return "handheld kernel"sv;
        }
        found = std::ranges::search(m_name, server);
        if (!found.empty()) {
            return "server kernel"sv;
        }
        found = std::ranges::search(m_name, next);
        if (!found.empty()) {
            return "next release"sv;
        }
        found = std::ranges::search(m_name, mainline);
        if (!found.empty()) {
            return "mainline branch"sv;
        }
        found = std::ranges::search(m_name, git);
        if (!found.empty()) {
            return "master branch"sv;
        }
        found = std::ranges::search(m_name, rc);
        if (!found.empty()) {
            return "release candidate"sv;
        }

        return "stable"sv;
    }
    std::string version() noexcept;

    bool is_installed() const noexcept;
    bool install() const noexcept;
    bool remove() const noexcept;
    /* clang-format off */
    constexpr bool is_update_available() const noexcept
    { return m_update; }

    inline const char* get_raw() const noexcept
    { return m_raw.c_str(); }

    inline std::string_view get_repo() const noexcept
    { return m_repo.c_str(); }

    inline std::string_view get_installed_db() const noexcept
    { return m_installed_db.c_str(); }
    /* clang-format on */

    static void commit_transaction() noexcept;

    static std::vector<Kernel> get_kernels(alpm_handle_t* handle) noexcept;

    static std::vector<std::string>& get_install_list() noexcept;
    static std::vector<std::string>& get_removal_list() noexcept;

 private:
    bool m_update{};

    std::string m_name{};
    std::string m_repo{"local"};
    std::string m_raw{};
    std::string m_installed_db{};
#ifdef ENABLE_AUR_KERNELS
    std::string m_version{};
    std::string m_name_headers{};
#endif

    alpm_pkg_t* m_pkg{nullptr};
    alpm_pkg_t* m_headers{nullptr};
    alpm_pkg_t* m_zfs_module{nullptr};
    alpm_pkg_t* m_nvidia_module{nullptr};
    alpm_pkg_t* m_nvidia_open_module{nullptr};
    alpm_handle_t* m_handle{nullptr};
};

#endif  // KERNEL_HPP
