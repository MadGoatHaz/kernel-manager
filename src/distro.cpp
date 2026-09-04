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

#include "distro.hpp"

#include <cctype>        // for tolower
#include <filesystem>    // for is_directory
#include <fstream>       // for ifstream
#include <iterator>      // for istreambuf_iterator
#include <system_error>  // for error_code

namespace {

// Trim surrounding whitespace (including a trailing CR from CRLF files),
// then strip one matching pair of surrounding quotes. os-release values
// are optionally quoted: NAME="EndeavourOS".
[[gnu::pure]] std::string_view clean_value(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
        value.remove_suffix(1);
    }
    if (value.size() >= 2) {
        const char first = value.front();
        if ((first == '"' && value.back() == '"') || (first == '\'' && value.back() == '\'')) {
            value.remove_prefix(1);
            value.remove_suffix(1);
        }
    }
    return value;
}

// Lowercased copy (os-release IDs are conventionally lowercase; be
// lenient about capitalization).
std::string lower(std::string_view value) {
    std::string out{};
    out.reserve(value.size());
    for (const char c : value) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

// Read the first value for `key` (exact key, no whitespace) from the
// os-release content, line by line. The value is quote/whitespace-cleaned
// into `out`. Returns false when the key is absent.
bool read_os_release_key(std::string_view content, std::string_view key, std::string& out) {
    size_t pos = 0;
    while (pos <= content.size()) {
        const size_t end            = content.find('\n', pos);
        const std::string_view line = (end == std::string_view::npos) ? content.substr(pos)
                                                                      : content.substr(pos, end - pos);
        if (line.size() > key.size() + 1
            && line.compare(0, key.size(), key) == 0
            && line[key.size()] == '=') {
            out.assign(clean_value(line.substr(key.size() + 1)));
            return true;
        }
        if (end == std::string_view::npos) {
            break;
        }
        pos = end + 1;
    }
    return false;
}

}  // namespace

std::string distro_name(DistroFamily family) {
    switch (family) {
    case DistroFamily::ARCH:
        return "Arch";
    case DistroFamily::ENDEAVOUROS:
        return "EndeavourOS";
    case DistroFamily::MANJARO:
        return "Manjaro";
    case DistroFamily::CACHYOS:
        return "CachyOS";
    case DistroFamily::GARUDA:
        return "Garuda";
    case DistroFamily::UNKNOWN:
        break;
    }
    return "Unknown";
}

DistroFamily detect_distro(const DistroProbe& probe) {
    if (!probe.read_os_release) {
        return DistroFamily::UNKNOWN;
    }
    const std::string content = probe.read_os_release();

    std::string id{};
    if (!read_os_release_key(content, "ID", id) || id.empty()) {
        return DistroFamily::UNKNOWN;
    }
    const std::string matched = lower(id);
    if (matched == "arch") {
        return DistroFamily::ARCH;
    }
    if (matched == "endeavouros") {
        return DistroFamily::ENDEAVOUROS;
    }
    if (matched == "manjaro") {
        return DistroFamily::MANJARO;
    }
    if (matched == "cachyos") {
        return DistroFamily::CACHYOS;
    }
    if (matched == "garuda") {
        return DistroFamily::GARUDA;
    }

    // ID is some other (possibly arch-based) distro: the space-separated
    // ID_LIKE family list decides. `arch` anywhere ⇒ arch-based.
    std::string id_like{};
    if (read_os_release_key(content, "ID_LIKE", id_like)) {
        std::string_view rest = id_like;
        while (!rest.empty()) {
            const size_t space           = rest.find(' ');
            const std::string_view token = (space == std::string_view::npos) ? rest : rest.substr(0, space);
            if (lower(token) == "arch") {
                return DistroFamily::ARCH;
            }
            if (space == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(space + 1);
        }
    }
    return DistroFamily::UNKNOWN;
}

DistroFamily detect_distro() {
    DistroProbe probe{};
    probe.read_os_release = [] {
        std::ifstream in{"/etc/os-release"};
        if (!in) {
            return std::string{};
        }
        std::string content{};
        std::string line{};
        while (std::getline(in, line)) {
            content += line;
            content += '\n';
        }
        return content;
    };
    return detect_distro(probe);
}

std::vector<std::string> preferred_initramfs_tools(DistroFamily family) {
    if (family == DistroFamily::ENDEAVOUROS) {
        // The blessed EOS repair tool first (it rebuilds the initramfs for
        // the installed kernel via kernel-install + dracut); dracut and
        // mkinitcpio remain the fallbacks.
        return {"reinstall-kernels", "dracut", "mkinitcpio"};
    }
    return {"dracut", "mkinitcpio"};
}

std::string bls_entries_dir(const std::function<bool(std::string_view)>& path_exists) {
    if (path_exists && path_exists("/efi/loader/entries")) {
        return "/efi/loader/entries";
    }
    if (path_exists && path_exists("/boot/loader/entries")) {
        return "/boot/loader/entries";
    }
    return {};
}

std::string bls_entries_dir() {
    std::function<bool(std::string_view)> is_dir = [](std::string_view path) noexcept {
        std::error_code ec{};
        return std::filesystem::is_directory(path, ec) && !ec;
    };
    return bls_entries_dir(is_dir);
}
