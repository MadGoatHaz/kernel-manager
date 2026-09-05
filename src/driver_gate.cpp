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

#include "driver_gate.hpp"

#include <array>         // for array
#include <cctype>        // for tolower
#include <cstdio>        // for popen, pclose
#include <filesystem>    // for path, is_directory, directory_iterator
#include <fstream>       // for ifstream
#include <iterator>      // for istreambuf_iterator
#include <system_error>  // for error_code

namespace driver_gate {
namespace {

    // Lowercased copy (lspci lines and the PCI vendor ID are matched
    // case-insensitively — the distro.cpp lower() precedent).
    [[nodiscard]] std::string lower(std::string_view value) {
        std::string out{};
        out.reserve(value.size());
        for (const char c : value) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return out;
    }

    // Trim trailing whitespace/CR (sysfs attribute files may carry a
    // trailing newline; the content itself is a bare hex ID).
    [[nodiscard]] std::string_view trim(std::string_view value) {
        while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n')) {
            value.remove_suffix(1);
        }
        return value;
    }

    // Read a small file's whole content ("" when absent/unreadable — the /sys
    // vendor files are a handful of bytes; nothing here streams).
    [[nodiscard]] std::string read_small_file(const std::filesystem::path& path) {
        std::ifstream in{path};
        if (!in) {
            return {};
        }
        return {(std::istreambuf_iterator<char>{in}), std::istreambuf_iterator<char>{}};
    }

    // The module's single popen helper (D2): runs one fixed command string
    // and captures its stdout. The commands are fixed literals chosen at the
    // call site (lspci, pacman -Siq, dkms status); the one parameterized call
    // (package_in_sync_db) passes a name restricted to the package-name
    // character class before it reaches the shell. On popen failure the
    // sentinel "-1" is returned and callers treat it as "no signal" (the
    // degradation contract), never as data.
    [[nodiscard]] std::string run_fixed_command(std::string_view command) {
        // NOLINTNEXTLINE(bugprone-command-processor) — by design: a shell is required to pipe the fixed tool output (lspci / pacman -Siq / dkms status); the command strings are compile-time literals or package-name-safe strings, so no user input reaches the shell. This is the third standing suppression (the other two are utils.cpp's popen and bootloader.cpp's `command -v`); per CONTRIBUTING it is specific, names the check, and carries its rationale.
        if (FILE* const pipe = popen(std::string{command}.c_str(), "r")) {
            std::string output{};
            char buffer[512];
            std::size_t got = 0;
            while ((got = std::fread(buffer, 1, sizeof buffer, pipe)) > 0) {
                output.append(buffer, got);
            }
            pclose(pipe);
            return output;
        }
        return "-1";
    }

    // Package-name-safe character class: the letters, digits, and the
    // ., +, -, _ that pacman package names use. The guard that keeps the one
    // parameterized shell string (pacman -Siq <name>) metacharacter-free.
    [[nodiscard]] bool is_pkg_name_safe(std::string_view name) {
        if (name.empty() || name.size() > 255) {
            return false;
        }
        for (const char c : name) {
            const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                || c == '.' || c == '+' || c == '-' || c == '_';
            if (!safe) {
                return false;
            }
        }
        return true;
    }

    // Invoke a probe predicate; an unbound predicate contributes no signal
    // (false) so a partially populated probe never throws (the ask()
    // precedent in bootloader.cpp).
    template <typename Predicate>
    [[nodiscard]] bool ask(const Predicate& predicate) {
        return static_cast<bool>(predicate) && predicate();
    }

    template <typename Predicate>
    [[nodiscard]] bool ask(const Predicate& predicate, std::string_view arg) {
        return static_cast<bool>(predicate) && predicate(arg);
    }

    // The gpu_names probe member through the same no-crash rule ("" when
    // unbound).
    [[nodiscard]] std::string ask_names(const std::function<std::string()>& names) {
        return static_cast<bool>(names) ? names() : std::string{};
    }

    // The D2 GPU keyword tables: curated, disjoint marketing-name tokens
    // (case-insensitive substring match over the raw lspci line). Chip
    // codenames are NOT usable — Pascal and Turing share the GP10x family
    // (GP102 is both the P100 and the RTX 2080 Ti) — so only marketing tokens
    // disambiguate; the chip prefixes ga10/ad10/gb20/tu10 are unambiguous
    // Turing-or-newer (gp10 is deliberately absent).
    constexpr std::array<std::string_view, 38> kTuringPlus{
        "gtx 16",
        "rtx 20",
        "rtx 30",
        "rtx 40",
        "rtx 50",
        "rtx a",
        "t1000",
        "t1400",
        "t2000",
        "t4000",
        "t6000",
        "t400",
        "t40 ",
        "t10 ",
        "t20 ",
        "t100 ",
        "a10 ",
        "a16 ",
        "a2 ",
        "a4 ",
        "a30 ",
        "a100",
        "a2000",
        "a3000",
        "a4000",
        "a5000",
        "a5500",
        "a6000",
        "ga10",
        "ad10",
        "gb20",
        "tu10",
        "titan v",
        "titan rt",
        "titan (2022)",
        "l40",
        "l4 ",
        "l2 ",
    };

    constexpr std::array<std::string_view, 36> kPreTuring{
        "gtx 4",
        "gtx 5",
        "gtx 6",
        "gtx 7",
        "gtx 8",
        "gtx 9",
        "gtx 10",
        "gt 4",
        "gt 5",
        "gt 6",
        "gt 7",
        "gt 8",
        "gt 9",
        "gt 10",
        "gk11",
        "gm20",
        "quadro p",
        "p40",
        "p600",
        "p100",
        "p2000",
        "p3000",
        "p4000",
        "p5000",
        "p6000",
        "titan x",
        "grid k",
        "tesla k",
        "k20",
        "k40",
        "k520",
        "k600",
        "k80",
        "m40",
        "m60",
        "m5500",
    };

    // The first non-UNKNOWN generation among the (raw multi-line) lspci
    // nvidia lines; "" or all-unknown lines -> UNKNOWN (the conservative
    // select_dkms_package fallback input).
    [[nodiscard]] GpuGeneration first_gpu_generation(std::string_view names) {
        std::size_t pos = 0;
        while (pos <= names.size()) {
            const std::size_t end       = names.find('\n', pos);
            const std::string_view line = (end == std::string_view::npos) ? names.substr(pos)
                                                                          : names.substr(pos, end - pos);
            if (!line.empty()) {
                const GpuGeneration gen = classify_gpu(line);
                if (gen != GpuGeneration::UNKNOWN) {
                    return gen;
                }
            }
            if (end == std::string_view::npos) {
                break;
            }
            pos = end + 1;
        }
        return GpuGeneration::UNKNOWN;
    }

    // The D7 persistent banner (exact tr() text; the Qt layer owns the
    // status-bar label). Shown on a decline (WARN_MIGRATE "proceed without"
    // / WARN_ONLY "proceed") and re-shown on every later decline.
    constexpr std::string_view kDriverBanner = "Precompiled nvidia driver detected — custom kernel will NOT have GPU acceleration";

    // The kernel reference for user-facing sentences: its name in quotes, or
    // the generic phrase when the target is unknown.
    [[nodiscard]] std::string kernel_ref(const GateTarget& target) {
        if (target.kernel.empty()) {
            return "the custom kernel";
        }
        return "the custom kernel '" + target.kernel + "'";
    }

}  // namespace

std::string derive_headers_pkg(std::string_view kernel) {
    if (kernel.empty()) {
        return {};
    }
    if (kernel.ends_with("-headers")) {
        return std::string{kernel};
    }
    return std::string{kernel} + "-headers";
}

GpuGeneration classify_gpu(std::string_view lspci_line) {
    const std::string line = lower(lspci_line);
    for (const std::string_view keyword : kTuringPlus) {
        if (line.contains(keyword)) {
            return GpuGeneration::TURING_PLUS;
        }
    }
    for (const std::string_view keyword : kPreTuring) {
        if (line.contains(keyword)) {
            return GpuGeneration::PRE_TURING;
        }
    }
    return GpuGeneration::UNKNOWN;
}

std::string select_dkms_package(GpuGeneration gen) {
    switch (gen) {
    case GpuGeneration::TURING_PLUS:
        return "nvidia-open-dkms";  // the open module supports Turing and newer
    case GpuGeneration::PRE_TURING:
        return "nvidia-dkms";  // the open module does NOT support pre-Turing
    case GpuGeneration::UNKNOWN:
        break;
    }
    return "nvidia-dkms";  // UNKNOWN: conservative (broadest GPU coverage)
}

bool nvidia_hardware_present() {
    // Walk /sys/bus/pci/devices/*/vendor, true iff any file's content is
    // "0x10de" (case-insensitive) — the NVIDIA PCI vendor ID. The C++
    // analog of `grep -qi 0x10de /sys/bus/pci/devices/*/vendor` (no
    // shell, no popen).
    const std::filesystem::path devices{"/sys/bus/pci/devices"};
    std::error_code ec{};
    if (!std::filesystem::is_directory(devices, ec) || ec) {
        return false;
    }
    auto it = std::filesystem::directory_iterator{devices, ec};
    if (ec) {
        return false;
    }
    const std::filesystem::directory_iterator end{};
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;  // unreadable entry mid-scan: stop
        }
        const std::string content = read_small_file(it->path() / "vendor");
        if (lower(trim(content)) == "0x10de") {
            return true;
        }
    }
    return false;
}

bool package_installed(std::string_view name) {
    // Local package-DB membership (D1.4): the /var/lib/pacman/local/
    // entries are `<name>-<version>`; the match requires the `name + "-"`
    // prefix AND a digit at the version head (a pacman version starts
    // with a digit) — so `nvidia` never matches `nvidia-open`/
    // `nvidia-utils`/`nvidia-dkms` (head 'o'/'u'/'d' is not a digit) and
    // `nvidia-open` never matches `nvidia-open-dkms` (the `-dkms` suffix
    // precedes the version head). The same source of truth as
    // alpm_db_get_pkg(localdb, name) in kernel.cpp — Qt-free, popen-free.
    if (name.empty() || !is_pkg_name_safe(name)) {
        return false;
    }
    const std::filesystem::path local_db{"/var/lib/pacman/local"};
    std::error_code ec{};
    if (!std::filesystem::is_directory(local_db, ec) || ec) {
        return false;
    }
    const std::string prefix{std::string{name} + "-"};
    auto it = std::filesystem::directory_iterator{local_db, ec};
    if (ec) {
        return false;
    }
    const std::filesystem::directory_iterator end{};
    for (; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        const std::string entry = it->path().filename().string();
        if (entry.size() > prefix.size() && entry.starts_with(prefix) && entry.at(prefix.size()) >= '0'
            && entry.at(prefix.size()) <= '9') {
            return true;
        }
    }
    return false;
}

bool dkms_driver_installed() {
    return package_installed("nvidia-dkms") || package_installed("nvidia-open-dkms");
}

bool precompiled_driver_installed() {
    return package_installed("nvidia") || package_installed("nvidia-open") || package_installed("nvidia-lts")
        || package_installed("nvidia-open-lts");
}

std::string gpu_names() {
    // D2 real probe: the nvidia lines of `lspci -nn` through the single
    // module-local popen helper (fixed command string). Degradation:
    // lspci absent / popen fail (the "-1" sentinel) or zero nvidia lines
    // -> "" -> every GPU classifies UNKNOWN -> the conservative
    // nvidia-dkms (a false "open" is never selected on an unknown GPU).
    std::string out = run_fixed_command("lspci -nn 2>/dev/null | grep -i nvidia");
    if (out == "-1" || out.empty()) {
        return {};
    }
    return out;
}

bool build_dir_exists(std::string_view kver) {
    // The kernel's build dir: /usr/lib/modules/<kver>/build — present iff
    // the <kver> headers package is installed.
    if (kver.empty() || !is_pkg_name_safe(kver)) {
        return false;
    }
    const std::filesystem::path path{"/usr/lib/modules/" + std::string{kver} + "/build"};
    std::error_code ec{};
    return std::filesystem::is_directory(path, ec) && !ec;
}

bool package_in_sync_db(std::string_view name) {
    // D6 real probe: `pacman -Siq <name>` non-empty -> the package is
    // available from a configured sync repo (no root needed). Shares the
    // single module-local popen helper; the name is restricted to the
    // package-name character class before it reaches the shell.
    if (!is_pkg_name_safe(name)) {
        return false;
    }
    const std::string out = run_fixed_command("pacman -Siq " + std::string{name} + " 2>/dev/null");
    return out != "-1" && !out.empty();
}

std::string dkms_status() {
    // Best-effort `dkms status` capture (D3 post-migration verify input):
    // the local-DB + build-dir facts are authoritative, so an absent dkms
    // tool (the "-1" sentinel / empty output) degrades to "" and never
    // counts against success.
    std::string out = run_fixed_command("dkms status 2>/dev/null");
    if (out == "-1" || out.empty()) {
        return {};
    }
    return out;
}

GateVerdict evaluate_gate(const GateTarget& target) {
    // The real probe wiring (the distro.cpp real-overload precedent):
    // every system fact behind the injectable GateProbe.
    GateProbe probe{};
    probe.nvidia_hardware   = nvidia_hardware_present;
    probe.gpu_names         = gpu_names;
    probe.package_installed = package_installed;
    probe.build_dir_exists  = build_dir_exists;
    probe.repo_available    = package_in_sync_db;
    return evaluate_gate(probe, target);
}

GateVerdict evaluate_gate(const GateProbe& probe, const GateTarget& target) {
    // The D3 decision table (pure — no filesystem, no shell). Unbound
    // predicates contribute no signal (false/""), so a partially
    // populated probe degrades to the conservative row, never a crash.
    const bool hardware = ask(probe.nvidia_hardware);
    if (!hardware) {
        return {};  // PROCEED: no nvidia hardware — nothing to gate
    }

    const auto installed   = [&](std::string_view name) { return ask(probe.package_installed, name); };
    const bool precompiled = installed("nvidia") || installed("nvidia-open") || installed("nvidia-lts")
        || installed("nvidia-open-lts");
    const bool dkms = installed("nvidia-dkms") || installed("nvidia-open-dkms");

    if (precompiled) {
        // WARN_MIGRATE — a precompiled driver is installed, with or
        // without a DKMS driver (the coexistence case is noted in the
        // text); the migration is idempotent (--needed) and pacman's
        // conflict resolution removes the precompiled package.
        GateVerdict verdict{};
        verdict.action          = GateAction::WARN_MIGRATE;
        verdict.headers_package = derive_headers_pkg(target.kernel);
        verdict.dkms_package    = select_dkms_package(first_gpu_generation(ask_names(probe.gpu_names)));
        verdict.migration_cmd   = "pacman -S --needed --asexplicit " + verdict.dkms_package
            + (verdict.headers_package.empty() ? std::string{} : " " + verdict.headers_package);
        verdict.banner = std::string{kDriverBanner};
        if (dkms) {
            verdict.message = "A precompiled and a DKMS nvidia driver are both installed. The precompiled one is built only for the exact kernel your distribution ships, so "
                + kernel_ref(target) + " will have no GPU acceleration from it. We recommend keeping only the DKMS driver '"
                + verdict.dkms_package + "': pacman will remove the precompiled package"
                + (verdict.headers_package.empty() ? std::string{}
                                                   : " and install '" + verdict.headers_package + "'")
                + ".";
        } else {
            verdict.message = "A precompiled nvidia driver is installed. It is built only for the exact kernel your distribution ships, so "
                + kernel_ref(target) + " will have no GPU acceleration from it — and on some systems the boot process requires a display driver, so the machine may fail to start. We recommend the DKMS driver '"
                + verdict.dkms_package + "' instead: it builds itself for every kernel, and pacman will remove the precompiled package"
                + (verdict.headers_package.empty() ? std::string{}
                                                   : " and install '" + verdict.headers_package + "'")
                + ".";
        }
        return verdict;
    }

    if (!dkms) {
        return {};  // PROCEED: a GPU with no nvidia driver is the user's choice
    }

    // DKMS driver installed: the header pairing decides (D3 rows 5-7).
    // "headers ok" = the build dir for the target kver (when known) or
    // the derived headers package itself; with kver == "" the check
    // degrades to the headers-package membership (a fresh flavor install
    // has no build dir yet — the pairing provides it).
    const std::string headers = derive_headers_pkg(target.kernel);
    const bool headers_ok     = (!target.kver.empty() && ask(probe.build_dir_exists, target.kver))
        || (!headers.empty() && installed(headers));
    if (headers_ok) {
        return {};  // PROCEED: DKMS driver + headers/build dir in place
    }
    if (!headers.empty() && ask(probe.repo_available, headers)) {
        GateVerdict verdict{};
        verdict.action          = GateAction::ENSURE_HEADERS;
        verdict.headers_package = headers;
        verdict.ensure_cmd      = "pacman -S --needed " + headers;
        verdict.message         = "The DKMS nvidia driver is installed, but the kernel headers for "
            + kernel_ref(target) + " are missing. '" + headers
            + "' will be installed alongside the kernel so the driver can build for it.";
        return verdict;
    }

    GateVerdict verdict{};
    verdict.action  = GateAction::WARN_ONLY;
    verdict.banner  = std::string{kDriverBanner};
    verdict.message = "The DKMS nvidia driver is installed, but the kernel headers for "
        + kernel_ref(target)
        + " are not available from any package repository. Without them the driver cannot build for this kernel, so you will have no GPU acceleration. You can still proceed, but the display may not work until the headers are provided some other way.";
    return verdict;
}

}  // namespace driver_gate
