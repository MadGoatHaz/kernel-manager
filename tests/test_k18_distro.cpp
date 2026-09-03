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

// Standalone unit test for the K18 distribution-awareness module (no CTest
// infra in this project; follows the "standalone g++ on the real sources"
// precedent of tests/run_k5.sh). The module is pure C++ (no Qt, no
// external deps), so this compiles it directly with the project's warning
// set and drives detect_distro() / preferred_initramfs_tools() /
// bls_entries_dir() with fake os-release probes only (no filesystem),
// asserting:
//   - each arch-family ID maps to its DistroFamily (case-insensitive,
//     quoted values handled)
//   - ID_LIKE=arch maps other arch-based distros to ARCH
//   - non-Arch families (fedora, gentoo, linuxmint/ID_LIKE=ubuntu, empty,
//     missing ID) are correctly rejected as UNKNOWN — out of domain
//   - an unbound probe is safe (UNKNOWN, no crash)
//   - preferred_initramfs_tools: ENDEAVOUROS leads with reinstall-kernels
//     (3 tools), every other family is {dracut, mkinitcpio}
//   - bls_entries_dir: /efi wins over /boot, /boot alone, neither → "",
//     unbound predicate → "" (no crash)
//   - distro_name() is non-empty (and exact) for all six values
//   - the real detect_distro() + bls_entries_dir() run without crashing

#include "distro.hpp"

#include <cstdio>   // for printf
#include <string>   // for string

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

// A probe whose read_os_release returns fixed content.
DistroProbe probe_of(const std::string& content) {
    DistroProbe probe{};
    probe.read_os_release = [content] { return content; };
    return probe;
}

}  // namespace

int main() {
    // ------------------------------------------------------------------
    // 1. Each arch-family ID maps to its DistroFamily.
    // ------------------------------------------------------------------
    check(detect_distro(probe_of("NAME=\"Arch Linux\"\nID=arch\nID_LIKE=\n")) == DistroFamily::ARCH,
          "ID=arch -> ARCH");
    check(detect_distro(probe_of("NAME=\"EndeavourOS\"\nID=endeavouros\nID_LIKE=arch\n")) == DistroFamily::ENDEAVOUROS,
          "ID=endeavouros -> ENDEAVOUROS");
    check(detect_distro(probe_of("ID=manjaro\nID_LIKE=arch\n")) == DistroFamily::MANJARO,
          "ID=manjaro -> MANJARO");
    check(detect_distro(probe_of("ID=cachyos\nID_LIKE=arch\n")) == DistroFamily::CACHYOS,
          "ID=cachyos -> CACHYOS");
    check(detect_distro(probe_of("ID=garuda\nID_LIKE=arch\n")) == DistroFamily::GARUDA,
          "ID=garuda -> GARUDA");

    // ------------------------------------------------------------------
    // 2. Lenient parsing: case-insensitive ID, quoted values, key order,
    //    CRLF, whitespace.
    // ------------------------------------------------------------------
    check(detect_distro(probe_of("ID=EndeavourOS\n")) == DistroFamily::ENDEAVOUROS,
          "ID=EndeavourOS (capitalized) -> ENDEAVOUROS");
    check(detect_distro(probe_of("ID=\"garuda\"\n")) == DistroFamily::GARUDA,
          "quoted ID=\"garuda\" -> GARUDA");
    check(detect_distro(probe_of("PRETTY_NAME=\"CachyOS\"\nID_LIKE=arch\nID=cachyos\n")) == DistroFamily::CACHYOS,
          "ID after other keys -> CACHYOS");
    check(detect_distro(probe_of("ID=arch\r\nID_LIKE=\r\n")) == DistroFamily::ARCH,
          "CRLF line endings -> ARCH");
    check(detect_distro(probe_of("ID = arch\n")) == DistroFamily::UNKNOWN,
          "space before '=' is not a valid key -> UNKNOWN");

    // ------------------------------------------------------------------
    // 3. ID_LIKE=arch maps other arch-based distros to ARCH.
    // ------------------------------------------------------------------
    check(detect_distro(probe_of("ID=voidlinux\nID_LIKE=arch\n")) == DistroFamily::ARCH,
          "ID_LIKE=arch -> ARCH (arch-based)");
    check(detect_distro(probe_of("ID=weird\nID_LIKE=arch manjaro\n")) == DistroFamily::ARCH,
          "ID_LIKE='arch manjaro' -> ARCH");
    check(detect_distro(probe_of("ID=weird\nID_LIKE=manjaro arch\n")) == DistroFamily::ARCH,
          "ID_LIKE='manjaro arch' -> ARCH (arch anywhere)");
    check(detect_distro(probe_of("ID=weird\nID_LIKE=\"arch\"\n")) == DistroFamily::ARCH,
          "quoted ID_LIKE=\"arch\" -> ARCH");
    check(detect_distro(probe_of("ID=weird\nID_LIKE=ARCH\n")) == DistroFamily::ARCH,
          "capitalized ID_LIKE=ARCH -> ARCH");
    check(detect_distro(probe_of("ID=weird\nID_LIKE=archlinux\n")) == DistroFamily::UNKNOWN,
          "ID_LIKE=archlinux (not a token) -> UNKNOWN");

    // ------------------------------------------------------------------
    // 4. Non-Arch families are correctly rejected (out of domain).
    // ------------------------------------------------------------------
    check(detect_distro(probe_of("ID=fedora\nID_LIKE=\n")) == DistroFamily::UNKNOWN,
          "ID=fedora -> UNKNOWN (out of domain)");
    check(detect_distro(probe_of("ID=gentoo\n")) == DistroFamily::UNKNOWN,
          "ID=gentoo (no ID_LIKE) -> UNKNOWN");
    check(detect_distro(probe_of("ID=linuxmint\nID_LIKE=ubuntu\n")) == DistroFamily::UNKNOWN,
          "ID=linuxmint ID_LIKE=ubuntu -> UNKNOWN (debian family)");
    check(detect_distro(probe_of("ID=opensuse\nID_LIKE=suse\n")) == DistroFamily::UNKNOWN,
          "ID=opensuse ID_LIKE=suse -> UNKNOWN");
    check(detect_distro(probe_of("")) == DistroFamily::UNKNOWN,
          "empty os-release -> UNKNOWN");
    check(detect_distro(probe_of("NAME=\"Something\"\nVERSION=1\n")) == DistroFamily::UNKNOWN,
          "no ID key at all -> UNKNOWN");

    // ------------------------------------------------------------------
    // 5. Probe safety: unbound predicate contributes no signal.
    // ------------------------------------------------------------------
    check(detect_distro(DistroProbe{}) == DistroFamily::UNKNOWN,
          "unbound probe -> UNKNOWN, no crash");

    // ------------------------------------------------------------------
    // 6. preferred_initramfs_tools: family-tuned preference order.
    // ------------------------------------------------------------------
    {
        const std::vector<std::string> eos = preferred_initramfs_tools(DistroFamily::ENDEAVOUROS);
        check(eos.size() == 3 && eos[0] == "reinstall-kernels" && eos[1] == "dracut" && eos[2] == "mkinitcpio",
              "ENDEAVOUROS -> {reinstall-kernels, dracut, mkinitcpio}");
        check(!eos.empty() && eos[0] == "reinstall-kernels",
              "ENDEAVOUROS tools lead with the blessed reinstall-kernels");
    }
    for (const DistroFamily family : {DistroFamily::ARCH,
                                      DistroFamily::MANJARO,
                                      DistroFamily::CACHYOS,
                                      DistroFamily::GARUDA,
                                      DistroFamily::UNKNOWN}) {
        const std::vector<std::string> tools = preferred_initramfs_tools(family);
        check(tools.size() == 2 && tools[0] == "dracut" && tools[1] == "mkinitcpio",
              (std::string{distro_name(family)} + " -> {dracut, mkinitcpio}").c_str());
    }

    // ------------------------------------------------------------------
    // 7. bls_entries_dir: ESP preferred over /boot; neither → "".
    // ------------------------------------------------------------------
    check(bls_entries_dir([](std::string_view path) { return path == "/efi/loader/entries"; }) == "/efi/loader/entries",
          "only /efi/loader/entries -> ESP dir");
    check(bls_entries_dir([](std::string_view path) { return path == "/boot/loader/entries"; }) == "/boot/loader/entries",
          "only /boot/loader/entries -> /boot dir");
    check(bls_entries_dir([](std::string_view) { return true; }) == "/efi/loader/entries",
          "both present -> /efi wins (ESP)");
    check(bls_entries_dir([](std::string_view) { return false; }) == "",
          "neither present -> empty string");
    check(bls_entries_dir(std::function<bool(std::string_view)>{}) == "",
          "unbound predicate -> empty string, no crash");

    // ------------------------------------------------------------------
    // 8. distro_name: non-empty and exact for all six values.
    // ------------------------------------------------------------------
    check(distro_name(DistroFamily::ARCH) == "Arch", "name: ARCH == \"Arch\"");
    check(distro_name(DistroFamily::ENDEAVOUROS) == "EndeavourOS", "name: ENDEAVOUROS == \"EndeavourOS\"");
    check(distro_name(DistroFamily::MANJARO) == "Manjaro", "name: MANJARO == \"Manjaro\"");
    check(distro_name(DistroFamily::CACHYOS) == "CachyOS", "name: CACHYOS == \"CachyOS\"");
    check(distro_name(DistroFamily::GARUDA) == "Garuda", "name: GARUDA == \"Garuda\"");
    check(distro_name(DistroFamily::UNKNOWN) == "Unknown", "name: UNKNOWN == \"Unknown\"");
    for (const DistroFamily family : {DistroFamily::ARCH,
                                      DistroFamily::ENDEAVOUROS,
                                      DistroFamily::MANJARO,
                                      DistroFamily::CACHYOS,
                                      DistroFamily::GARUDA,
                                      DistroFamily::UNKNOWN}) {
        check(!distro_name(family).empty(), "name: non-empty for every enum value");
    }

    // ------------------------------------------------------------------
    // 9. Real probe smoke: runs on this machine (read-only), must yield
    //    a valid enum + a BLS dir in the known set without crashing.
    // ------------------------------------------------------------------
    {
        const DistroFamily real = detect_distro();
        check(real == DistroFamily::ARCH || real == DistroFamily::ENDEAVOUROS
              || real == DistroFamily::MANJARO || real == DistroFamily::CACHYOS
              || real == DistroFamily::GARUDA || real == DistroFamily::UNKNOWN,
              "real: detect_distro() returns a valid enum");
        check(!distro_name(real).empty(), "real: distro_name() of the live detection is non-empty");
        std::printf("INFO: live detection on this machine -> %s\n", distro_name(real).c_str());

        const std::string bls = bls_entries_dir();
        check(bls == "/efi/loader/entries" || bls == "/boot/loader/entries" || bls.empty(),
              "real: bls_entries_dir() is in the known set");
        std::printf("INFO: live BLS entries dir -> '%s'\n", bls.c_str());
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
