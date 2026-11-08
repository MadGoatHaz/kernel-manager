// Copyright (C) 2022-2026 Vladislav Nepogodin
//
// This file is part of kernel-manager.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#ifndef POOPEN_UTILS_HPP
#define POOPEN_UTILS_HPP

// A small, Qt-free, header-only popen helper (T-060). The three modules
// that capture shell output (utils::exec, kernel_info::run_command,
// driver_gate::run_fixed_command) previously each carried their own
// popen/fread/pclose loop with slightly different buffer sizes and caps.
// This consolidates them into one inline function so the security-
// critical shell-execution path is defined in a single place.
//
// The function is header-only (inline) and has no Qt dependency, so it
// can be used by both Qt code (utils.cpp) and Qt-free code
// (kernel_info.cpp, driver_gate.cpp). The caller is responsible for
// ensuring the command string is safe (charset-guarded, no raw user
// text) — see the NOLINT comments at each call site.
//
// Returns the captured stdout (trailing newlines stripped), or the
// sentinel "-1" on popen failure. `max_bytes` caps the output length
// (the caller chooses a cap appropriate to the command's expected
// output size; the default of 1 MiB covers every known use).

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

namespace popen_utils {

[[nodiscard]] inline std::string capture(std::string_view command, std::size_t max_bytes = 1u << 20u) {
    // NOLINTNEXTLINE(bugprone-command-processor) — by design: this is the
    // app's single command-execution primitive (pacman/makepkg/git/lspci/
    // dkms through a shell); the command strings are built at each call
    // site from vetted, single-quoted, charset-guarded inputs, never raw
    // user text. Each call site carries its own specific NOLINT explaining
    // its particular inputs.
    if (FILE* const pipe = ::popen(std::string{command}.c_str(), "r")) {
        std::string result{};
        result.reserve(std::min(max_bytes, std::size_t{4096}));
        std::array<char, 4096> buffer{};
        while (result.size() < max_bytes) {
            const std::size_t got = std::fread(buffer.data(), 1, buffer.size(), pipe);
            if (got == 0) {
                break;
            }
            const std::size_t room = max_bytes - result.size();
            result.append(buffer.data(), got > room ? room : got);
        }
        ::pclose(pipe);
        // Strip trailing newlines/CRs (the common shell-output shape);
        // keep everything else intact (multi-line output, internal newlines).
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
            result.pop_back();
        }
        return result;
    }
    return "-1";
}

}  // namespace popen_utils

#endif  // POOPEN_UTILS_HPP
