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

#ifndef UTILS_HPP
#define UTILS_HPP

#include "alpm_utils.hpp"
#include "string_utils.hpp"

#include <filesystem>   // for path
#include <string>       // for string
#include <string_view>  // for string_view
#include <vector>       // for vector

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wold-style-cast"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wnull-dereference"
#pragma GCC diagnostic ignored "-Wsuggest-attribute=pure"
#endif

#include <QString>

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace utils {

[[nodiscard]] auto read_whole_file(std::string_view filepath) noexcept -> std::string;
bool write_to_file(std::string_view filepath, std::string_view data) noexcept;
std::string exec(std::string_view command) noexcept;
[[nodiscard]] std::string fix_path(std::string&& path) noexcept;

// Runs a command in a terminal, escalates using pkexec if escalate is true
int runCmdTerminal(QString cmd, bool escalate) noexcept;

int run_process(std::string_view program, const std::vector<std::string>& args) noexcept;
void prepare_git_repo(const std::filesystem::path& parent_dir, const std::filesystem::path& repo_path, std::string_view clone_url) noexcept;

void prepare_build_environment() noexcept;
void restore_clean_environment(std::vector<std::string>& previously_set_options, std::string_view all_set_values) noexcept;

}  // namespace utils

#endif  // UTILS_HPP
