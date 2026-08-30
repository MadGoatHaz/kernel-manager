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

#ifndef BOOT_INSTRUCTIONS_HPP
#define BOOT_INSTRUCTIONS_HPP

#include "bootloader.hpp"  // for Bootloader

#include <string>  // for string
#include <vector>  // for vector

// Ordered, human-readable post-install steps for selecting the given
// kernel at the next boot, chosen from the detected Bootloader. The
// result is a pure function of its two inputs (no filesystem access,
// no commands run): the kernel package base is substituted into the
// step text, and a mandatory closing note about the /boot/vmlinuz
// binary and initramfs regeneration is always appended last, so any
// bootloader yields a non-empty, ordered, display-ready list.
[[gnu::pure]] [[nodiscard]] std::vector<std::string> instructions_for(Bootloader bl,
                                                                      const std::string& kernel_pkgbase);

#endif  // BOOT_INSTRUCTIONS_HPP
