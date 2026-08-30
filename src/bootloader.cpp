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

#include "bootloader.hpp"

#include <sys/wait.h>  // for WIFEXITED, WEXITSTATUS

#include <cstdio>      // for popen, pclose
#include <filesystem>  // for path, exists, is_directory, directory_iterator
#include <string>      // for string

namespace {

// Real path probe: a file or directory exists (non-throwing).
bool real_path_exists(std::string_view path) noexcept {
    std::error_code ec{};
    return std::filesystem::exists(std::filesystem::path{path}, ec) && !ec;
}

// Real UKI probe: a regular *.efi file directly in the directory. A
// missing or unreadable directory yields false, never an exception.
bool real_dir_has_uki(std::string_view dir) noexcept {
    const std::filesystem::path path{dir};
    std::error_code ec{};
    if (!std::filesystem::is_directory(path, ec) || ec) {
        return false;
    }
    auto it = std::filesystem::directory_iterator{path, ec};
    if (ec) {
        return false;
    }
    const std::filesystem::directory_iterator end{};
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;  // unreadable entry mid-scan: stop
        }
        if (it->is_regular_file(ec) && !ec && it->path().extension() == ".efi") {
            return true;
        }
    }
    return false;
}

// Real command probe: the command is on PATH. Mirrors the `command -v`
// popen pattern used via utils::exec() (see the has_chwd probe in
// src/kernel.cpp), but stays dependency-free so this module compiles in
// the standalone unit-test harness alone.
bool real_command_exists(std::string_view command) noexcept {
    const std::string shell_command{"command -v " + std::string{command} + " >/dev/null 2>&1"};
    if (FILE* const pipe = popen(shell_command.c_str(), "r")) {
        const int status = pclose(pipe);
        return status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    return false;
}

// Invoke a probe predicate; an unbound predicate contributes no signal
// (treated as "not found") so a partially populated probe never throws.
template <typename Predicate>
bool ask(const Predicate& predicate, std::string_view arg) {
    return static_cast<bool>(predicate) && predicate(arg);
}

}  // namespace

std::string bootloader_name(Bootloader bl) {
    switch (bl) {
        case Bootloader::UKI:
            return "Unified Kernel Image (UKI)";
        case Bootloader::SYSTEMD_BOOT:
            return "systemd-boot";
        case Bootloader::GRUB:
            return "GRUB";
        case Bootloader::UNKNOWN:
            break;
    }
    return "Unknown";
}

Bootloader detect_bootloader(const BootloaderProbe& probe) {
    // 1. UKI — a .efi image in a systemd-boot UKI directory.
    if (ask(probe.dir_has_uki, "/boot/EFI/Linux") || ask(probe.dir_has_uki, "/efi/EFI/Linux")) {
        return Bootloader::UKI;
    }

    // 2. systemd-boot — bootctl plus loader configuration or entries.
    if (ask(probe.command_exists, "bootctl")
        && (ask(probe.path_exists, "/boot/loader/loader.conf")
            || ask(probe.path_exists, "/efi/loader/loader.conf")
            || ask(probe.path_exists, "/boot/loader/entries"))) {
        return Bootloader::SYSTEMD_BOOT;
    }

    // 3. GRUB — config file, grub.d scripts, or the tooling.
    if (ask(probe.path_exists, "/boot/grub/grub.cfg")
        || ask(probe.path_exists, "/etc/grub.d")
        || ask(probe.command_exists, "grub-mkconfig")
        || ask(probe.command_exists, "grub-editenv")) {
        return Bootloader::GRUB;
    }

    // 4. No signal.
    return Bootloader::UNKNOWN;
}

Bootloader detect_bootloader() {
    BootloaderProbe probe{};
    probe.path_exists = real_path_exists;
    probe.command_exists = real_command_exists;
    probe.dir_has_uki = real_dir_has_uki;
    return detect_bootloader(probe);
}
