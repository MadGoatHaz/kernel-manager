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

#ifndef ALPM_UTILS_HPP
#define ALPM_UTILS_HPP

#include <cstdint>      // for int32_t
#include <functional>   // for function
#include <string>       // for string
#include <string_view>  // for string_view

#include <alpm.h>

namespace utils {

// Single named constants for the alpm filesystem locations (root and package
// DB dir), shared by every parse_alpm call site.
inline constexpr std::string_view alpm_root   = "/";
inline constexpr std::string_view alpm_libdir = "/var/lib/pacman/";

alpm_handle_t* parse_alpm(std::string_view root, std::string_view dbpath, alpm_errno_t* err) noexcept;
std::int32_t release_alpm(alpm_handle_t* handle, alpm_errno_t* err) noexcept;

// Where a pre-compiled package is installable from: a pacman sync repo
// (core/extra/community/multilib or third-party such as cachyos/
// chaotic-aur/liquorix) or the AUR.
enum class PackageSource { PACMAN_REPO, AUR };

// Classify a repo name: "aur" → AUR, everything else → a pacman sync repo.
// A repo section missing from /etc/pacman.conf is not an error here — it
// simply yields a negative availability result (is_package_in_sync_db).
// (Inlined: trivially pure.)
[[nodiscard]] inline PackageSource classify_repo(std::string_view repo) noexcept {
    return repo == "aur" ? PackageSource::AUR : PackageSource::PACMAN_REPO;
}

// Command executor with the utils::exec contract (stdout string, "-1" on
// spawn failure). Injected where the availability probes run so the AUR
// path is unit-testable without a real paru.
using ExecFn = std::function<std::string(std::string_view)>;

// AUR availability (no root): true iff `paru` is on PATH and
// `paru --aur -Si <pkg>` produces stdout output (a missing package prints
// its error to stderr only and stays silent on stdout). Without paru — or
// when the probe fails — this gracefully returns false. Read-only AUR
// query, safe to call repeatedly.
[[nodiscard]] bool is_aur_package_available(const ExecFn& exec_fn, std::string_view pkg) noexcept;

// Pacman sync-DB availability (no root): true iff the handle (created by
// parse_alpm) has the named repo section registered and the package exists
// in it (alpm_db_get_pkg). A null handle or a repo section missing from
// /etc/pacman.conf yields false. Read-only DB access, safe to call
// repeatedly.
[[nodiscard]] bool is_package_in_sync_db(alpm_handle_t* handle, std::string_view repo, std::string_view pkg) noexcept;

// Package-availability helper (no root): decides whether a pre-compiled
// kernel package is installable. "aur" → the AUR probe above driven by
// utils::exec; any pacman repo → a fresh parse_alpm handle queried via
// is_package_in_sync_db and released on the way out. Each call creates and
// releases its own handle, so repeated calls are safe.
[[nodiscard]] bool is_package_available(std::string_view pkg, std::string_view repo) noexcept;

}  // namespace utils

#endif  // ALPM_UTILS_HPP
