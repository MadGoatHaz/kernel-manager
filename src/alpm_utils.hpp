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

#ifndef ALPM_UTILS_HPP
#define ALPM_UTILS_HPP

#include <cstdint>      // for int32_t
#include <string_view>  // for string_view

#include <alpm.h>

namespace utils {

alpm_handle_t* parse_alpm(std::string_view root, std::string_view dbpath, alpm_errno_t* err) noexcept;
std::int32_t release_alpm(alpm_handle_t* handle, alpm_errno_t* err) noexcept;

}  // namespace utils

#endif  // ALPM_UTILS_HPP
