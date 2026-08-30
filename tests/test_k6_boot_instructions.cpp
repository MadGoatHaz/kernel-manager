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

// Standalone unit test for the K6 boot-instructions module (no CTest
// infra in this project; follows the "standalone g++ on the real
// sources" precedent of tests/run_k5.sh). The module is pure C++ (no
// Qt, no external deps), so this compiles it with the project's
// warning set and asserts, for each of the four bootloaders:
//   - a non-empty, ordered step list with the bootloader's key
//     strings (grub-mkconfig line, loader-entries / bootctl
//     set-default lines, UKI auto-detect line, unknown fallback line)
//   - the mandatory /boot/vmlinuz-<pkg> + mkinitcpio note, present
//     and LAST for all four
//   - no un-substituted placeholder token in any step
//   - a live smoke: instructions_for(detect_bootloader(), "linux")

#include "boot_instructions.hpp"
#include "bootloader.hpp"

#include <cstddef>  // for size_t
#include <cstdio>   // for printf
#include <string>   // for string
#include <vector>   // for vector

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::printf("PASS: %s\n", what);
    } else {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// Index of the first step containing `needle`, or a sentinel if absent.
size_t index_of(const std::vector<std::string>& steps, const std::string& needle) {
    constexpr size_t sentinel = static_cast<size_t>(-1);
    for (size_t i = 0; i < steps.size(); ++i) {
        if (contains(steps[i], needle)) {
            return i;
        }
    }
    return sentinel;
}

// The mandatory closing note, parameterised by the kernel package base.
std::string expected_note(const std::string& pkg) {
    return "The kernel binary is at `/boot/vmlinuz-" +
           pkg + "`; the initramfs is regenerated automatically on install (ALPM hook) or via `sudo mkinitcpio -P`";
}

// Invariants every bootloader's list must satisfy: non-empty, ends
// with the note (which names /boot/vmlinuz-<pkg>), no placeholders.
void check_common_invariants(const char* label, const std::vector<std::string>& steps, const std::string& pkg) {
    const std::string l{label};
    check(!steps.empty(), (l + ": non-empty").c_str());
    if (steps.empty()) {
        return;
    }
    check(steps.back() == expected_note(pkg), (l + ": last step is the vmlinuz/mkinitcpio note").c_str());
    check(contains(steps.back(), "/boot/vmlinuz-" + pkg), (l + ": note names /boot/vmlinuz-<pkg>").c_str());
    bool no_placeholder = true;
    for (const std::string& step : steps) {
        if (contains(step, "<P>") || contains(step, "<kernel>")) {
            no_placeholder = false;
        }
    }
    check(no_placeholder, (l + ": no un-substituted placeholder token").c_str());
}

}  // namespace

int main() {
    // A non-default pkgbase so the substitution is visible in output.
    const std::string pkg = "linux-zen";

    // ------------------------------------------------------------------
    // 1. GRUB: 3 steps + note, ordered (regen -> select -> default).
    // ------------------------------------------------------------------
    {
        const std::vector<std::string> steps = instructions_for(Bootloader::GRUB, pkg);
        check_common_invariants("GRUB", steps, pkg);
        check(steps.size() == 4, "GRUB: exactly 3 steps + note");
        if (steps.size() == 4) {
            check(contains(steps[0], "grub-mkconfig"), "GRUB[0]: re-run grub-mkconfig");
            check(contains(steps[1], "'" + pkg + "'"), "GRUB[1]: select '<pkg>' from the GRUB menu");
            check(contains(steps[2], "GRUB_DEFAULT"), "GRUB[2]: make default via GRUB_DEFAULT");
            check(index_of(steps, "grub-mkconfig") == 0 && index_of(steps, "Reboot and select") == 1,
                  "GRUB: ordered (regen before reboot-select)");
        }
    }

    // ------------------------------------------------------------------
    // 2. SYSTEMD_BOOT: 5 steps + note (UKI shortcut, loader entry,
    //    verify, select, set-default).
    // ------------------------------------------------------------------
    {
        const std::vector<std::string> steps = instructions_for(Bootloader::SYSTEMD_BOOT, pkg);
        check_common_invariants("systemd-boot", steps, pkg);
        check(steps.size() == 6, "systemd-boot: exactly 5 steps + note");
        if (steps.size() == 6) {
            check(contains(steps[1], "/boot/loader/entries/" + pkg + ".conf"), "systemd-boot[1]: loader entry file");
            check(contains(steps[1], "linux /boot/vmlinuz-" + pkg), "systemd-boot[1]: `linux` line");
            check(contains(steps[1], "initrd /boot/initramfs-" + pkg + ".img"), "systemd-boot[1]: `initrd` line");
            check(contains(steps[2], "bootctl"), "systemd-boot[2]: verify with bootctl");
            check(contains(steps[4], "bootctl set-default " + pkg), "systemd-boot[4]: set default via bootctl");
            check(index_of(steps, "loader/entries") < index_of(steps, "bootctl"), "systemd-boot: ordered (entry before verify)");
        }
    }

    // ------------------------------------------------------------------
    // 3. UKI: 1 step + note (auto-detect).
    // ------------------------------------------------------------------
    {
        const std::vector<std::string> steps = instructions_for(Bootloader::UKI, pkg);
        check_common_invariants("UKI", steps, pkg);
        check(steps.size() == 2, "UKI: exactly 1 step + note");
        if (steps.size() == 2) {
            check(contains(steps[0], "Unified Kernel Image"), "UKI[0]: auto-detect line");
            check(contains(steps[0], "'" + pkg + "'"), "UKI[0]: select '<pkg>'");
        }
    }

    // ------------------------------------------------------------------
    // 4. UNKNOWN: 2 steps + note (boot menu, then both regenerator paths).
    // ------------------------------------------------------------------
    {
        const std::vector<std::string> steps = instructions_for(Bootloader::UNKNOWN, pkg);
        check_common_invariants("unknown", steps, pkg);
        check(steps.size() == 3, "unknown: exactly 2 steps + note");
        if (steps.size() == 3) {
            check(contains(steps[0], "boot menu"), "unknown[0]: choose from the boot menu");
            check(contains(steps[1], "regenerate"), "unknown[1]: fallback regenerate line");
            check(contains(steps[1], "grub-mkconfig"), "unknown[1]: names the GRUB regenerator");
        }
    }

    // ------------------------------------------------------------------
    // 5. Live smoke: drive the real detector on this machine (read-only)
    //    and require a non-empty, note-terminated list.
    // ------------------------------------------------------------------
    {
        const Bootloader real = detect_bootloader();
        const std::vector<std::string> steps = instructions_for(real, "linux");
        check(!steps.empty(), "live: non-empty for the detected bootloader");
        check(steps.back() == expected_note("linux"), "live: ends with the note");
        std::printf("INFO: live detection -> %s (%zu steps)\n", bootloader_name(real).c_str(), steps.size());
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
