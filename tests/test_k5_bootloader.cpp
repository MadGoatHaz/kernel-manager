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

// Standalone unit test for the K5 bootloader detection module (no CTest
// infra in this project; follows the "standalone g++ on the real
// sources" precedent of tests/run_chunk2.sh). The module is pure C++
// (no Qt, no external deps), so this compiles it directly with the
// project's warning set and drives detect_bootloader() with fake
// probes only (no filesystem, no command probes), asserting:
//   - each heuristic branch returns its bootloader (UKI / systemd-boot
//     / GRUB / UNKNOWN)
//   - the priority order (UKI > systemd-boot > GRUB)
//   - bootctl is trusted with no loader signal when there is no strong
//     GRUB signal (ESP unreadable), and yields GRUB when there is one
//   - strong GRUB = /boot/grub/grub.cfg, or grub-mkconfig +
//     /etc/default/grub; weak leftovers (/etc/grub.d, bare
//     grub-mkconfig, bare grub-editenv) are NOT signals
//   - a partially populated (or empty) probe is safe and yields UNKNOWN
//   - bootloader_name() is non-empty (and exact) for all four values
//   - the real detect_bootloader() runs without crashing on this box

#include "bootloader.hpp"

#include <cstdio>  // for printf

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

// A probe where every predicate answers "no signal". Cases below
// override individual predicates to force a branch.
BootloaderProbe no_signal_probe() {
    BootloaderProbe probe{};
    probe.path_exists = [](std::string_view) { return false; };
    probe.command_exists = [](std::string_view) { return false; };
    probe.dir_has_uki = [](std::string_view) { return false; };
    return probe;
}

}  // namespace

int main() {
    // ------------------------------------------------------------------
    // 1. UKI (highest priority).
    // ------------------------------------------------------------------
    {
        auto probe = no_signal_probe();
        probe.dir_has_uki = [](std::string_view dir) { return dir == "/boot/EFI/Linux"; };
        check(detect_bootloader(probe) == Bootloader::UKI, "UKI: /boot/EFI/Linux with a .efi -> UKI");
    }
    {
        auto probe = no_signal_probe();
        probe.dir_has_uki = [](std::string_view dir) { return dir == "/efi/EFI/Linux"; };
        check(detect_bootloader(probe) == Bootloader::UKI, "UKI: /efi/EFI/Linux with a .efi -> UKI");
    }

    // ------------------------------------------------------------------
    // 2. SYSTEMD_BOOT: bootctl AND a loader signal.
    // ------------------------------------------------------------------
    {
        auto probe = no_signal_probe();
        probe.command_exists = [](std::string_view cmd) { return cmd == "bootctl"; };
        probe.path_exists = [](std::string_view path) { return path == "/boot/loader/loader.conf"; };
        check(detect_bootloader(probe) == Bootloader::SYSTEMD_BOOT, "systemd-boot: bootctl + /boot/loader/loader.conf");
    }
    {
        auto probe = no_signal_probe();
        probe.command_exists = [](std::string_view cmd) { return cmd == "bootctl"; };
        probe.path_exists = [](std::string_view path) { return path == "/efi/loader/loader.conf"; };
        check(detect_bootloader(probe) == Bootloader::SYSTEMD_BOOT, "systemd-boot: bootctl + /efi/loader/loader.conf");
    }
    {
        auto probe = no_signal_probe();
        probe.command_exists = [](std::string_view cmd) { return cmd == "bootctl"; };
        probe.path_exists = [](std::string_view path) { return path == "/boot/loader/entries"; };
        check(detect_bootloader(probe) == Bootloader::SYSTEMD_BOOT, "systemd-boot: bootctl + /boot/loader/entries");
    }
    {
        auto probe = no_signal_probe();
        probe.command_exists = [](std::string_view cmd) { return cmd == "bootctl"; };
        probe.path_exists = [](std::string_view path) { return path == "/efi/loader/entries"; };
        check(detect_bootloader(probe) == Bootloader::SYSTEMD_BOOT, "systemd-boot: bootctl + /efi/loader/entries");
    }
    {
        // bootctl present but no loader signal and no GRUB signal (the
        // ESP is unreadable): bootctl is systemd-boot specific, so the
        // presence of the command alone is trusted.
        auto probe = no_signal_probe();
        probe.command_exists = [](std::string_view cmd) { return cmd == "bootctl"; };
        check(detect_bootloader(probe) == Bootloader::SYSTEMD_BOOT, "systemd-boot: bootctl, unreadable ESP (no loader signal) -> SYSTEMD_BOOT");
    }
    {
        // bootctl present but no loader signal and a strong GRUB signal
        // (grub.cfg): the GRUB signal wins — this is not systemd-boot.
        auto probe = no_signal_probe();
        probe.command_exists = [](std::string_view cmd) { return cmd == "bootctl"; };
        probe.path_exists = [](std::string_view path) { return path == "/boot/grub/grub.cfg"; };
        check(detect_bootloader(probe) == Bootloader::GRUB, "systemd-boot: bootctl + no loader signal + grub.cfg -> GRUB");
    }
    {
        // Same via the second strong signal: grub-mkconfig +
        // /etc/default/grub (but no loader paths visible).
        auto probe = no_signal_probe();
        probe.command_exists = [](std::string_view cmd) { return cmd == "bootctl" || cmd == "grub-mkconfig"; };
        probe.path_exists = [](std::string_view path) { return path == "/etc/default/grub"; };
        check(detect_bootloader(probe) == Bootloader::GRUB, "systemd-boot: bootctl + no loader signal + grub-mkconfig & /etc/default/grub -> GRUB");
    }

    // ------------------------------------------------------------------
    // 3. GRUB: strong signals only; weak leftovers are not signals.
    // ------------------------------------------------------------------
    {
        auto probe = no_signal_probe();
        probe.path_exists = [](std::string_view path) { return path == "/boot/grub/grub.cfg"; };
        check(detect_bootloader(probe) == Bootloader::GRUB, "GRUB: /boot/grub/grub.cfg exists");
    }
    {
        auto probe = no_signal_probe();
        probe.command_exists = [](std::string_view cmd) { return cmd == "grub-mkconfig"; };
        probe.path_exists = [](std::string_view path) { return path == "/etc/default/grub"; };
        check(detect_bootloader(probe) == Bootloader::GRUB, "GRUB: grub-mkconfig on PATH + /etc/default/grub exists");
    }
    {
        // Strong signal plus the fwupd leftover: the leftover must not
        // change the outcome (it is not itself a signal).
        auto probe = no_signal_probe();
        probe.command_exists = [](std::string_view cmd) { return cmd == "grub-mkconfig"; };
        probe.path_exists = [](std::string_view path) { return path == "/etc/default/grub" || path == "/etc/grub.d"; };
        check(detect_bootloader(probe) == Bootloader::GRUB, "GRUB: strong signal + /etc/grub.d leftover stays GRUB");
    }
    {
        // Weak leftovers alone: NOT GRUB.
        auto probe = no_signal_probe();
        probe.path_exists = [](std::string_view path) { return path == "/etc/grub.d"; };
        check(detect_bootloader(probe) == Bootloader::UNKNOWN, "GRUB: /etc/grub.d alone (fwupd leftover) -> UNKNOWN");
    }
    {
        auto probe = no_signal_probe();
        probe.command_exists = [](std::string_view cmd) { return cmd == "grub-mkconfig"; };
        check(detect_bootloader(probe) == Bootloader::UNKNOWN, "GRUB: grub-mkconfig alone (no /etc/default/grub) -> UNKNOWN");
    }
    {
        auto probe = no_signal_probe();
        probe.command_exists = [](std::string_view cmd) { return cmd == "grub-editenv"; };
        check(detect_bootloader(probe) == Bootloader::UNKNOWN, "GRUB: grub-editenv alone -> UNKNOWN");
    }
    {
        // /etc/default/grub without grub-mkconfig: still not a strong
        // signal (grub-editenv is not counted).
        auto probe = no_signal_probe();
        probe.command_exists = [](std::string_view cmd) { return cmd == "grub-editenv"; };
        probe.path_exists = [](std::string_view path) { return path == "/etc/default/grub"; };
        check(detect_bootloader(probe) == Bootloader::UNKNOWN, "GRUB: grub-editenv + /etc/default/grub (no grub-mkconfig) -> UNKNOWN");
    }

    // ------------------------------------------------------------------
    // 4. UNKNOWN: no signal at all.
    // ------------------------------------------------------------------
    check(detect_bootloader(no_signal_probe()) == Bootloader::UNKNOWN, "UNKNOWN: no signal at all");

    // ------------------------------------------------------------------
    // 5. Priority order: first match wins.
    // ------------------------------------------------------------------
    {
        // UKI + GRUB both present -> UKI.
        auto probe = no_signal_probe();
        probe.dir_has_uki = [](std::string_view) { return true; };
        probe.path_exists = [](std::string_view path) { return path == "/boot/grub/grub.cfg"; };
        check(detect_bootloader(probe) == Bootloader::UKI, "priority: UKI beats GRUB when both present");
    }
    {
        // UKI + systemd-boot both present -> UKI.
        auto probe = no_signal_probe();
        probe.dir_has_uki = [](std::string_view) { return true; };
        probe.command_exists = [](std::string_view cmd) { return cmd == "bootctl"; };
        probe.path_exists = [](std::string_view path) { return path == "/boot/loader/loader.conf"; };
        check(detect_bootloader(probe) == Bootloader::UKI, "priority: UKI beats systemd-boot when both present");
    }
    {
        // systemd-boot + GRUB both present -> systemd-boot.
        auto probe = no_signal_probe();
        probe.command_exists = [](std::string_view cmd) { return cmd == "bootctl" || cmd == "grub-mkconfig"; };
        probe.path_exists = [](std::string_view path) { return path == "/boot/loader/loader.conf" || path == "/boot/grub/grub.cfg"; };
        check(detect_bootloader(probe) == Bootloader::SYSTEMD_BOOT, "priority: systemd-boot beats GRUB when both present");
    }

    // ------------------------------------------------------------------
    // 6. Probe safety: unbound predicates contribute no signal.
    // ------------------------------------------------------------------
    {
        BootloaderProbe probe{};  // all three std::function members empty
        check(detect_bootloader(probe) == Bootloader::UNKNOWN, "empty probe (no predicates bound) -> UNKNOWN, no crash");
    }
    {
        // Only dir_has_uki bound: the other heuristics must not see it.
        auto probe = no_signal_probe();
        probe.dir_has_uki = [](std::string_view) { return true; };
        check(detect_bootloader(probe) == Bootloader::UKI, "partially populated probe still detects UKI");
    }

    // ------------------------------------------------------------------
    // 7. bootloader_name: non-empty and exact for all four values.
    // ------------------------------------------------------------------
    check(bootloader_name(Bootloader::UKI) == "Unified Kernel Image (UKI)", "name: UKI == \"Unified Kernel Image (UKI)\"");
    check(bootloader_name(Bootloader::SYSTEMD_BOOT) == "systemd-boot", "name: SYSTEMD_BOOT == \"systemd-boot\"");
    check(bootloader_name(Bootloader::GRUB) == "GRUB", "name: GRUB == \"GRUB\"");
    check(bootloader_name(Bootloader::UNKNOWN) == "Unknown", "name: UNKNOWN == \"Unknown\"");
    for (const Bootloader bl : {Bootloader::UKI, Bootloader::SYSTEMD_BOOT, Bootloader::GRUB, Bootloader::UNKNOWN}) {
        check(!bootloader_name(bl).empty(), "name: non-empty for every enum value");
    }

    // ------------------------------------------------------------------
    // 8. Real probe smoke: runs on this machine (read-only), must yield
    //    a valid enum and a non-empty name without crashing.
    // ------------------------------------------------------------------
    {
        const Bootloader real = detect_bootloader();
        check(real == Bootloader::UKI || real == Bootloader::SYSTEMD_BOOT || real == Bootloader::GRUB || real == Bootloader::UNKNOWN,
              "real: detect_bootloader() returns a valid enum");
        check(!bootloader_name(real).empty(), "real: bootloader_name() of the live detection is non-empty");
        std::printf("INFO: live detection on this machine -> %s\n", bootloader_name(real).c_str());
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
