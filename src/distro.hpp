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

#ifndef DISTRO_HPP
#define DISTRO_HPP

#include <functional>   // for function
#include <string>       // for string
#include <string_view>  // for string_view
#include <vector>       // for vector

// The Arch-family distribution detected from /etc/os-release (ID +
// ID_LIKE). kernel-manager's domain is the pacman/libalpm family, so
// non-Arch distributions (fedora, debian, gentoo, ...) are UNKNOWN —
// correctly out of scope, not a detection failure.
enum class DistroFamily { ARCH, ENDEAVOUROS, MANJARO, CACHYOS, GARUDA, UNKNOWN };

// Human-readable name of a detected family (UI-facing text, the
// bootloader_name() analog).
[[nodiscard]] [[gnu::pure]] std::string distro_name(DistroFamily family);

// Injection point for the detection: returns the raw /etc/os-release
// content (empty string when the file is absent/unreadable). Unit tests
// drive detect_distro() with fake content only — no filesystem access.
struct DistroProbe {
    std::function<std::string()> read_os_release;
};

// Pure detection over the injected probe. The `ID` value selects the
// family: arch→ARCH, endeavouros→ENDEAVOUROS, manjaro→MANJARO,
// cachyos→CACHYOS, garuda→GARUDA (case-insensitive, surrounding quotes
// stripped). When ID is none of those, the space-separated `ID_LIKE`
// list is scanned for `arch` → ARCH (any other arch-based distro), else
// UNKNOWN. A missing/empty ID or an unbound probe yields UNKNOWN.
[[nodiscard]] DistroFamily detect_distro(const DistroProbe& probe);

// Real detection: reads /etc/os-release from disk.
[[nodiscard]] DistroFamily detect_distro();

// The preferred initramfs tools for a family, in preference order: the
// first tool that exists on the system is the one to run. ENDEAVOUROS
// leads with the blessed `reinstall-kernels` repair tool; every other
// family (including UNKNOWN) prefers dracut over mkinitcpio. The
// postinstall tail still command-v-probes each tool, so this ordering is
// a soft input — the tail works even if this layer is absent.
[[nodiscard]] [[gnu::pure]] std::vector<std::string> preferred_initramfs_tools(DistroFamily family);

// The systemd-boot BLS entries directory on this machine: the pure form
// takes a path-exists predicate and returns the first candidate that
// exists (ESP first: /efi/loader/entries, then /boot/loader/entries), or
// the empty string when neither exists (no systemd-boot BLS layout — the
// kernel/loader is elsewhere, e.g. GRUB or a UKI). An unbound predicate
// yields the empty string, never a crash.
[[nodiscard]] std::string bls_entries_dir(const std::function<bool(std::string_view)>& path_exists);

// Real bls_entries_dir(): std::filesystem probes (the bootloader module's
// ESP-mount convention, /efi vs /boot).
[[nodiscard]] std::string bls_entries_dir();

#endif  // DISTRO_HPP
