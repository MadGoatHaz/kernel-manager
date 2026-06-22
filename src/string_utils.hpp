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

#ifndef STRING_UTILS_HPP
#define STRING_UTILS_HPP

#include <algorithm>    // for transform, for_each
#include <ranges>       // for ranges::*
#include <span>         // for span
#include <string>       // for string
#include <string_view>  // for string_view
#include <utility>      // for forward
#include <vector>       // for vector

namespace utils {

/// @brief Make a split view from a string into multiple lines based on a delimiter.
/// @param str The string to split.
/// @param delim The delimiter to split the string.
/// @return A range view representing the split lines.
constexpr auto make_split_view(std::string_view str, char delim = '\n') noexcept {
    constexpr auto functor = [](auto&& rng) {
        return std::string_view(&*rng.begin(), static_cast<size_t>(std::ranges::distance(rng)));
    };
    constexpr auto second = [](auto&& rng) { return rng != ""; };

    return str
        | std::ranges::views::split(delim)
        | std::ranges::views::transform(functor)
        | std::ranges::views::filter(second);
}

inline constexpr std::size_t replace_all(std::string& inout, std::string_view what, std::string_view with) noexcept {
    std::size_t count{};
    std::size_t pos{};
    while (std::string::npos != (pos = inout.find(what.data(), pos, what.length()))) {
        inout.replace(pos, what.length(), with.data(), with.length());
        pos += with.length(), ++count;
    }
    return count;
}

inline constexpr std::size_t remove_all(std::string& inout, std::string_view what) noexcept {
    return replace_all(inout, what, "");
}

/// @brief Split a string into multiple lines based on a delimiter.
/// @param str The string to split.
/// @param delim The delimiter to split the string.
/// @return A vector of strings representing the split lines.
constexpr auto make_multiline(std::string_view str, char delim = '\n') noexcept -> std::vector<std::string> {
    return [&]() constexpr {
        std::vector<std::string> lines{};
        std::ranges::for_each(utils::make_split_view(str, delim), [&](auto&& rng) { lines.emplace_back(rng); });
        return lines;
    }();
}

/// @brief Split a string into views of multiple lines based on a delimiter.
/// @param str The string to split.
/// @param delim The delimiter to split the string.
/// @return A vector of string views representing the split lines.
constexpr auto make_multiline_view(std::string_view str, char delim = '\n') noexcept -> std::vector<std::string_view> {
    return [&]() constexpr {
        std::vector<std::string_view> lines{};
        std::ranges::for_each(utils::make_split_view(str, delim), [&](auto&& rng) { lines.emplace_back(rng); });
        return lines;
    }();
}

/// @brief Join a vector of strings into a single string using a delimiter.
/// @param lines The lines to join.
/// @param delim The delimiter to join the lines.
/// @return The joined lines as a single string.
template <std::ranges::range Range>
constexpr auto join_vec(Range&& lines, std::string_view delim) noexcept -> std::string {
    return std::forward<Range>(lines) | std::ranges::views::join_with(delim) | std::ranges::to<std::string>();
}

}  // namespace utils

#endif  // STRING_UTILS_HPP
