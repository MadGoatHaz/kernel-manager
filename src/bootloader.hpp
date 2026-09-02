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

#ifndef BOOTLOADER_HPP
#define BOOTLOADER_HPP

#include <functional>   // for function
#include <string>       // for string
#include <string_view>  // for string_view

// The boot manager detected on the system, used to present the right
// post-install boot-selection guidance. Detection is a pure function of
// the injected BootloaderProbe, so unit tests drive it with fakes (no
// filesystem, no command probes); detect_bootloader() wires the real
// std::filesystem + `command -v` probes.
enum class Bootloader { UKI, SYSTEMD_BOOT, GRUB, UNKNOWN };

// Human-readable name of a detected bootloader (UI-facing text).
[[gnu::pure]] std::string bootloader_name(Bootloader bl);

// Injection point for the detection heuristics. Every predicate answers
// one question the heuristics ask; an unbound (empty) predicate
// contributes no signal, so a partially populated probe is safe.
struct BootloaderProbe {
    std::function<bool(std::string_view path)> path_exists;     // file or directory exists
    std::function<bool(std::string_view cmd)> command_exists;   // command on PATH (`command -v`)
    std::function<bool(std::string_view dir)> dir_has_uki;      // a regular *.efi file in dir
};

// Pure detection (first match wins): UKI > systemd-boot > GRUB > UNKNOWN.
//   1. UKI          — dir_has_uki("/boot/EFI/Linux") or dir_has_uki("/efi/EFI/Linux")
//   2. strong GRUB  — path_exists("/boot/grub/grub.cfg") OR (command_exists(
//                     "grub-mkconfig") AND path_exists("/etc/default/grub")).
//                     Weak leftovers (/etc/grub.d, bare grub-editenv) are NOT
//                     signals: they exist on non-GRUB systems (fwupd).
//   3. SYSTEMD_BOOT — command_exists("bootctl") AND (loader.conf under
//                     /boot or /efi, or entries dir under /boot or /efi, OR
//                     no strong GRUB signal — bootctl is systemd-boot
//                     specific, so with the ESP unreadable its presence
//                     alone is trusted over a missing loader path)
//   4. GRUB         — strong GRUB (step 2)
//   5. UNKNOWN      — no signal
Bootloader detect_bootloader(const BootloaderProbe& probe);

// Real detection: std::filesystem probes for paths/UKI + a popen
// `command -v` probe for commands (the utils::exec() pattern, kept
// dependency-free in this module).
Bootloader detect_bootloader();

#endif  // BOOTLOADER_HPP
