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

#include "boot_instructions.hpp"

#include <string>  // for string
#include <utility> // for move
#include <vector>  // for vector

namespace {

// Mandatory closing note appended to every bootloader's step list:
// where the kernel binary lives, and how the initramfs comes back
// after an install (the `-P` there is the literal mkinitcpio flag,
// not a placeholder).
std::string final_note(const std::string& pkgbase) {
    return "The kernel binary is at `/boot/vmlinuz-" + pkgbase +
           "`; the initramfs is regenerated automatically on install (ALPM hook) or via `sudo mkinitcpio -P`";
}

}  // namespace

std::vector<std::string> instructions_for(Bootloader bl, const std::string& kernel_pkgbase) {
    const std::string pkg{kernel_pkgbase};
    std::vector<std::string> steps{};

    switch (bl) {
        case Bootloader::GRUB:
            // Regenerate the config so the new /boot/vmlinuz-<pkg> shows
            // up, pick it from the menu, and optionally make it default.
            steps.push_back("Re-run the GRUB config: `sudo grub-mkconfig -o /boot/grub/grub.cfg`");
            steps.push_back("Reboot and select '" + pkg + "' from the GRUB menu");
            steps.push_back("To make it the default: set `GRUB_DEFAULT` in `/etc/default/grub`, then re-run `grub-mkconfig`");
            break;
        case Bootloader::SYSTEMD_BOOT:
            // A UKI needs nothing; otherwise hand-write the loader entry
            // (title + linux + initrd lines), verify with bootctl, pick
            // it at boot, and optionally set it as the default.
            steps.push_back("If a UKI is enabled for this kernel, its `.efi` in `/boot/EFI/Linux/` is auto-detected — just reboot and select it");
            steps.push_back("Otherwise create `/boot/loader/entries/" + pkg +
                            ".conf` with: title, `linux /boot/vmlinuz-" + pkg +
                            "`, `initrd /boot/initramfs-" + pkg + ".img`");
            steps.push_back("Verify with `bootctl`");
            steps.push_back("Reboot and select '" + pkg + "'");
            steps.push_back("To set the default: `sudo bootctl set-default " + pkg + "`");
            break;
        case Bootloader::UKI:
            // The firmware/systemd-boot picks the .efi up by itself.
            steps.push_back("The Unified Kernel Image is auto-detected by the firmware/systemd-boot — reboot and select '" + pkg + "'");
            break;
        case Bootloader::UNKNOWN:
            // Generic guidance: try the boot menu, and if the kernel is
            // missing, name both common regenerator paths.
            steps.push_back("Reboot and choose '" + pkg + "' from your boot menu");
            steps.push_back("If it does not appear, regenerate your bootloader config (GRUB: `sudo grub-mkconfig -o /boot/grub/grub.cfg`; systemd-boot: ensure a loader entry or UKI exists)");
            break;
    }

    // Invariant: every bootloader's list ends with the same note.
    steps.push_back(final_note(pkg));
    return steps;
}
