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

#include "install_kernel.hpp"

#include "aur_kernel.hpp"        // for detail::install_aur_kernels (the AUR build path)
#include "boot_instructions.hpp" // for instructions_for
#include "utils.hpp"             // for runCmdTerminal (default runner), exec (paru probe + .PKGINFO)

#include <algorithm>    // for ranges::sort
#include <ctime>        // for time, localtime_r, strftime (install log timestamps)
#include <filesystem>   // for directory_iterator, path, absolute
#include <fstream>      // for ofstream (install log appends)
#include <string>       // for string, to_string
#include <string_view>  // for string_view
#include <system_error> // for error_code
#include <utility>      // for move
#include <vector>       // for vector

#include <fmt/compile.h>
#include <fmt/core.h>

namespace {

// The real command runner: the shared terminal path (an escalated
// command is wrapped in pkexec rootshell — see utils.cpp).
int run_real_command(const std::string& cmd, bool escalate) noexcept {
    return utils::runCmdTerminal(QString::fromStdString(cmd), escalate);
}

// A step that installs a package (pacman/paru/makepkg — the custom
// build runs makepkg through build_helper.sh), as opposed to the
// post-install tail (the DKMS build + initramfs regeneration + GRUB
// config).
[[gnu::pure]] bool is_package_install_step(const std::string& cmd) {
    return cmd.starts_with("pacman ") || cmd.starts_with("paru ") || cmd.starts_with("makepkg ")
           || cmd.starts_with(KM_HELPER_DIR "/build_helper.sh");
}

// Shell-quote a string for embedding in the terminal command line: wrap
// it in single quotes and escape every embedded single quote as '\''
// (the run_cmd_async CWD-safe quoting precedent, utils.cpp). The value
// arrives at the receiving script as one intact word.
std::string shell_quote(std::string_view s) {
    std::string quoted{"'"};
    for (const char c : s) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

// The bootloader name as postinstall_tail.sh's `case "$BL"` consumes it
// (the chunk C contract: the lowercase systemd-boot / grub / uki /
// unknown). The UI-facing bootloader_name() returns different strings
// ("GRUB", "Unified Kernel Image (UKI)", "Unknown") that would fall into
// the tail's out-of-contract default (no BLS/grub check at all), so map
// the enum to the tail's exact names.
[[gnu::pure]] std::string tail_bootloader_name(Bootloader bl) {
    switch (bl) {
        case Bootloader::SYSTEMD_BOOT: return "systemd-boot";
        case Bootloader::GRUB:         return "grub";
        case Bootloader::UKI:          return "uki";
        case Bootloader::UNKNOWN:      return "unknown";
    }
    return "unknown";
}

// The verdict's log/UI label (D-E): the honest 3-state name the result
// line and the dialogs carry.
[[gnu::pure]] std::string verdict_label(PostinstallVerdict verdict) {
    switch (verdict) {
        case PostinstallVerdict::BOOT_SAFE:               return "BOOT_SAFE";
        case PostinstallVerdict::INSTALLED_NOT_BOOT_SAFE: return "INSTALLED_NOT_BOOT_SAFE";
        case PostinstallVerdict::INSTALLATION_FAILED:     return "INSTALLATION_FAILED";
    }
    return "UNKNOWN";
}

// The single consolidated post-install tail every precompiled install
// gets (chunk E, phase b): ONE escalated postinstall_tail.sh step (the
// script produced by chunks A/B/C) that, in ONE root session, syncs the
// kernel drivers, regenerates the initramfs for the new <KVER>, verifies
// boot-safety and exits with the 3-state verdict rc. `pkgname` is the
// installed package (the dir flow's .PKGINFO pkgname, the repo/AUR flow's
// table package) and `kver` the new kernel's version (the dir flow's
// .PKGINFO version; the repo/AUR flow passes the EMPTY version, which the
// tail then derives from the local DB itself). This replaces the old
// three inline fire-and-forget steps (driver rebuild / initramfs / GRUB
// refresh).
void append_postinstall_tail(std::vector<InstallStep>& steps, Bootloader bl,
                             std::string_view pkgname, std::string_view kver) {
    steps.push_back(
        {KM_HELPER_DIR "/postinstall_tail.sh " + shell_quote(pkgname) + " " + shell_quote(kver) + " "
         + shell_quote(tail_bootloader_name(bl)),
         true});
}

// The built-package suffix (D2: the literal .pkg.tar.zst per the user
// request; a PKGEXT-from-/etc/makepkg.conf generalization, after
// conf-window.cpp's get_pkgext_value_from_makepkgconf, is documented
// future work, not this cycle). NOTE: std::filesystem::path::extension()
// returns only the trailing ".zst" component, so the suffix is matched
// whole — never via extension().
inline constexpr std::string_view kPkgSuffix = ".pkg.tar.zst";

// Trim surrounding whitespace and one pair of surrounding quotes from a
// .PKGINFO value (the file-local copy of utils.cpp's
// strip_pkgbuild_value — that one is not exported; D2 note).
std::string strip_pkginfo_value(std::string_view value) {
    const auto left  = value.find_first_not_of(" \t");
    const auto right = value.find_last_not_of(" \t");
    if (left == std::string_view::npos) {
        return std::string{};
    }
    value = value.substr(left, right - left + 1);
    if (value.size() >= 2) {
        const auto first = value.front();
        if ((first == '"' || first == '\'') && value.back() == first) {
            value = value.substr(1, value.size() - 2);
        }
    }
    return std::string{value};
}

// The first .PKGINFO `key = value` field of the extracted content (""
// when the key is absent): the fields are read independently, so a
// missing pkgrel degrades to a bare pkgver version (the lenient parse
// k14 asserts).
std::string pkginfo_field(std::string_view content, std::string_view key) {
    const auto prefix = std::string{key} + " = ";
    for (const auto line : utils::make_multiline_view(content)) {
        if (line.starts_with(prefix)) {
            return strip_pkginfo_value(line.substr(prefix.size()));
        }
    }
    return std::string{};
}

// ── Install logging (the real-runner path; see install_from_directory) ──

// The step's short label for the log: the first two whitespace-separated
// words of the command ("pacman -U", "dracut -f", "grub-mkconfig -o") —
// enough to identify the step without the long, quote-laden package
// paths. A `sh -c '...'` wrapper (the guarded tail steps) is stripped
// first so the label names the inner command ("pacman -Q",
// "command -v"), not the wrapper.
std::string short_label(std::string_view cmd) {
    std::string_view inner = cmd;
    constexpr std::string_view kShcWrapper = "sh -c '";
    if (inner.starts_with(kShcWrapper) && inner.ends_with('\'') && inner.size() > kShcWrapper.size() + 1) {
        inner = inner.substr(kShcWrapper.size(), inner.size() - kShcWrapper.size() - 1);
    }
    int words = 0;
    for (std::size_t i = 0; i < inner.size(); ++i) {
        if (inner[i] == ' ' && ++words == 2) {
            return std::string{inner.substr(0, i)};
        }
    }
    return std::string{inner};
}

// The install log's two timestamp forms for one epoch-seconds value,
// both in local time: the file-name form (the log's
// "install-YYYYMMDD-HHMMSS" name) and the human form (the header's
// "Date: YYYY-MM-DD HH:MM:SS" line). The strftime formats are literals
// at the call (a non-literal format trips -Wformat-nonliteral).
struct LogTimestamps {
    std::string file;
    std::string human;
};
LogTimestamps format_log_times(std::time_t t) {
    LogTimestamps ts{};
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64]{};
    const std::size_t n_file = std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    ts.file = {buf, n_file};
    const std::size_t n_human = std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    ts.human = {buf, n_human};
    return ts;
}

// A package file's identity for the log header: the filename minus the
// .pkg.tar.zst suffix and minus the trailing -<arch> (best effort —
// "linux-cachyos-custom-7.2.2-1-x86_64.pkg.tar.zst" ⇒
// "linux-cachyos-custom-7.2.2-1").
std::string package_identity(const std::filesystem::path& pkg) {
    std::string base = pkg.filename().string();
    if (base.size() > kPkgSuffix.size() && base.ends_with(kPkgSuffix)) {
        base.erase(base.size() - kPkgSuffix.size());
    }
    static constexpr std::string_view kKnownArchs[] = {"-x86_64", "-i686", "-aarch64", "-loongarch64", "-riscv64"};
    for (const auto& arch : kKnownArchs) {
        if (base.size() > arch.size() && base.ends_with(arch)) {
            base.erase(base.size() - arch.size());
            break;
        }
    }
    return base;
}

// Append text to the install log (a trailing newline is added if it
// lacks one). A failed append is a no-op: the log is best-effort and the
// install itself never depends on it.
void append_log(const std::string& log_path, std::string_view text) {
    std::ofstream out(log_path, std::ios::app);
    if (out) {
        out << text;
        if (!text.empty() && text.back() != '\n') {
            out << '\n';
        }
    }
}

}  // namespace

std::vector<InstallStep> plan_steps(const KnownKernel& kernel, Bootloader bl) {
    std::vector<InstallStep> steps{};

    if (kernel.precompiled_available && !kernel.install_package.empty()) {
        if (kernel.install_repo == "aur") {
            // A precompiled AUR package: the paru install is not
            // privileged (an AUR build runs as the user). No makepkg
            // fallback step is emitted here — install_kernel() resolves
            // a missing paru at run time (see below).
            steps.push_back({"paru -S --needed " + kernel.install_package, false});
        } else {
            // Every documented pacman repo (core/extra/cachyos/
            // chaotic-aur/liquorix, or a third-party one the user
            // added) installs through pacman.
            steps.push_back({"pacman -S --needed " + kernel.install_package, true});
        }
        // Phase b: the single consolidated tail. The repo/AUR flow passes
        // an EMPTY kver — the tail derives it from the local DB of the
        // package phase a just installed (chunk A's awk, never uname).
        append_postinstall_tail(steps, bl, kernel.install_package, "");
        return steps;
    }

    if (kernel.buildable && !kernel.default_source.empty()) {
        // No precompiled package: the custom build path — the same
        // makepkg command the app already runs for AUR kernels (the
        // source repo is prepared by the existing build environment,
        // see detail::install_aur_kernels / the Configure flow), wrapped
        // in build_helper.sh (auto-imports a missing GPG key on
        // "unknown public key" and retries).
        steps.push_back({KM_HELPER_DIR "/build_helper.sh -sicf --cleanbuild", false});
    }
    return steps;
}

InstallKernelResult install_kernel(const KnownKernel& kernel, CommandRunner runner, Bootloader bl) {
    InstallKernelResult result{};
    // The boot-selection steps describe the detected bootloader + the
    // kernel, not the install outcome: they are filled on every run so
    // the caller can show them even after a failure.
    result.boot_instructions = instructions_for(bl, kernel.name);

    if (!kernel.precompiled_available || kernel.install_package.empty()) {
        // A buildable-only kernel: there is no precompiled package to
        // install; the Build-custom (Configure) flow is the install
        // path (fail gracefully, run nothing).
        result.ok = false;
        result.error = "No precompiled package for '" + kernel.name +
                       "'; use the Build-custom (Configure) flow to build it";
        return result;
    }

    const bool use_default_runner = !runner;
    if (!runner) {
        runner = run_real_command;
    }

    auto steps = plan_steps(kernel, bl);

    // AUR parity (the same pattern detail::install_aur_kernels uses): a
    // precompiled AUR install needs paru; without it the build path
    // takes over instead of failing. Only reachable with the real
    // runner — an injected runner asserts the exact paru sequence.
    if (use_default_runner
        && kernel.install_repo == "aur"
        && !steps.empty()
        && steps.front().cmd.starts_with("paru ")) {
        if (utils::exec("command -v paru").empty()) {
            std::vector<std::string> packages{kernel.install_package};
            detail::install_aur_kernels(packages);
            steps.erase(steps.begin());  // keep the post-install tail
        }
    }

    // The execution loop (D-E, the 3-state verdict): the phase-(a) package
    // install runs first; a failure there ⇒ INSTALLATION_FAILED and the tail
    // never runs. The phase-(b) consolidated tail is identified by its
    // postinstall_tail.sh command (not by position — the AUR-erase path may
    // leave it as the only step); its REAL rc (the sentinel-captured exit
    // code, chunk D) IS the verdict: 0 = BOOT_SAFE, != 0 = INSTALLED_NOT_BOOT_SAFE.
    for (const auto& step : steps) {
        const int rc = runner(step.cmd, step.escalate);
        if (step.cmd.starts_with(KM_HELPER_DIR "/postinstall_tail.sh")) {
            // Phase (b): the tail's real rc is the verdict. Never a false
            // SUCCESS — the H3 fire-and-forget gap is closed by the sentinel.
            if (rc == 0) {
                result.verdict = PostinstallVerdict::BOOT_SAFE;
                result.ok = true;
            } else {
                result.verdict = PostinstallVerdict::INSTALLED_NOT_BOOT_SAFE;
                result.ok = false;
                result.error = "Post-install tail not boot-safe (exit code " + std::to_string(rc) + "): " + step.cmd;
            }
        } else if (rc != 0) {
            // Phase (a): the package install itself failed (a missing/
            // disabled repo, an unresolved conflict, a Polkit denial).
            result.verdict = PostinstallVerdict::INSTALLATION_FAILED;
            result.ok = false;
            result.error = "Command failed (exit code " + std::to_string(rc) + "): " + step.cmd;
            return result;  // the tail never runs; graceful, never a crash
        }
    }

    return result;
}

InstallPlan plan_install(std::string_view kernel_name) {
    const std::string name{km::kernel_name_from_raw(kernel_name)};

    // The curated entry, or the K1 fallback for an unknown name (its own
    // package as the build source, no precompiled path, never
    // unbuildable).
    KnownKernel kernel{};
    if (const auto entry = km::find_kernel(name); entry.has_value()) {
        kernel = **entry;  // find_kernel hands out optional<const KnownKernel*>
    } else {
        kernel.name = name;
        kernel.default_source = name;
        kernel.install_package = name;
        kernel.install_repo = "";
        kernel.precompiled_available = false;
        kernel.buildable = true;
    }

    InstallPlan plan{};
    plan.kernel = name;
    plan.package = kernel.install_package;
    plan.repo = kernel.install_repo;
    plan.precompiled = kernel.precompiled_available && !kernel.install_package.empty();

    if (!plan.precompiled) {
        // No precompiled package: the plan carries no commands, only the
        // note pointing at the Build-custom flow.
        plan.note = "No precompiled package for '" + name +
                    "'; use the Build-custom (Configure) flow to build it";
        return plan;
    }

    // Split the ordered plan into the package install commands and the
    // post-install tail (computed for the live-detected bootloader).
    for (const auto& step : plan_steps(kernel, detect_bootloader())) {
        if (is_package_install_step(step.cmd)) {
            plan.install_cmds.push_back(step.cmd);
        } else {
            plan.postinstall_cmds.push_back(step.cmd);
        }
    }
    return plan;
}

bool execute_plan(const InstallPlan& plan, std::string& error_out, CommandRunner runner) {
    // Rebuild the table entry the plan was derived from (an unknown
    // name took the K1 fallback in plan_install, so this mirrors its
    // data) and delegate to install_kernel() — one execution path,
    // per-step escalation flags and the AUR parity included.
    KnownKernel kernel{};
    kernel.name = plan.kernel;
    kernel.default_source = plan.kernel;
    kernel.install_package = plan.package;
    kernel.install_repo = plan.repo;
    kernel.precompiled_available = plan.precompiled;
    kernel.buildable = true;

    const InstallKernelResult result = install_kernel(kernel, std::move(runner));
    error_out = result.error;
    return result.ok;
}

std::vector<std::string> boot_instructions_for(std::string_view kernel_name) {
    return instructions_for(detect_bootloader(), std::string{km::kernel_name_from_raw(kernel_name)});
}

std::vector<std::filesystem::path> list_local_packages(std::string_view dir) {
    std::vector<std::filesystem::path> pkgs{};
    std::error_code ec{};
    const auto begin = std::filesystem::directory_iterator(dir, ec);
    if (ec) {
        return pkgs;  // a missing/unreadable dir holds nothing; the caller reports
    }
    // Non-recursive: a makepkg build dir holds the packages at its root
    // (the post-build flow's layout; nested per-source trees belong to
    // the Configure flow, not this one).
    for (auto it = begin; it != std::filesystem::directory_iterator{}; ++it) {
        if (it->path().filename().string().ends_with(kPkgSuffix)) {
            pkgs.push_back(it->path());
        }
    }
    // Sorted by filename: deterministic command strings + install order.
    std::ranges::sort(pkgs, [](const auto& a, const auto& b) { return a.filename() < b.filename(); });
    return pkgs;
}

bool read_pkginfo(const std::filesystem::path& pkg, std::string& name_out, std::string& version_out) {
    // Extract the .PKGINFO straight from the archive (no temp file, the
    // build artifact stays untouched; the tar + zstd tools are verified
    // present, D2). popen failure ⇒ exec returns its "-1" sentinel and a
    // non-archive ⇒ tar fails behind 2>/dev/null: both leave no pkgname
    // below, so any failure degrades to false.
    const auto content = utils::exec(
        fmt::format(FMT_COMPILE("tar --zstd -xOf '{}' .PKGINFO 2>/dev/null"), pkg.string()));

    const auto name = pkginfo_field(content, "pkgname");
    if (name.empty()) {
        return false;
    }
    const auto pkgver = pkginfo_field(content, "pkgver");
    const auto pkgrel = pkginfo_field(content, "pkgrel");  // optional in the format: a missing one degrades to the bare pkgver
    name_out = name;
    version_out = pkgver + (pkgrel.empty() ? "" : "-" + pkgrel);
    return true;
}

DirInstallResult install_from_directory(std::string_view dir, CommandRunner runner, Bootloader bl) {
    DirInstallResult result{};

    // The D2 guard: no built packages in the dir ⇒ nothing to install,
    // and the runner is never called (zero commands, no terminal).
    const auto pkgs = list_local_packages(dir);
    if (pkgs.empty()) {
        result.ok = false;
        result.error = "no *.pkg.tar.zst packages found in '" + std::string{dir} + "'";
        return result;  // boot_instructions stays empty: there is no kernel to point at
    }

    // The installed kernel's identity: the first package whose .PKGINFO
    // parses to a non-headers name (a dir may hold the kernel + its
    // headers pair; the boot instructions need the kernel base), else
    // the first filename — the best-effort fallback strips only the
    // .pkg.tar.zst suffix, so it keeps the -<version>-<arch> tail,
    // which the display text tolerates; the install itself never
    // depends on this parse (D2).
    std::string name{};
    std::string version{};
    for (const auto& pkg : pkgs) {
        std::string pkg_name{};
        std::string pkg_version{};
        if (read_pkginfo(pkg, pkg_name, pkg_version) && !pkg_name.ends_with("-headers")) {
            name = pkg_name;
            version = pkg_version;
            break;
        }
    }
    if (name.empty()) {
        const auto filename = pkgs.front().filename().string();
        name = filename.substr(0, filename.size() - kPkgSuffix.size());
    }

    // The boot-selection steps describe the detected bootloader + the
    // kernel, not the install outcome: they are filled before execution
    // (the install_kernel pattern) so the caller can show them even
    // after a failure.
    result.boot_instructions = instructions_for(bl, name);
    result.name = name;
    result.version = version;

    const bool use_default_runner = !runner;
    if (!runner) {
        runner = run_real_command;  // the shared pkexec terminal path
    }

    // The plan: one escalated pacman -U of the explicit single-quoted
    // ABSOLUTE paths — no shell glob (D2: the 0c918d4 pkexec-CWD-reset
    // rationale, the root shell starts in $HOME so relative paths
    // break; quoting keeps a spaced name one word instead of
    // word-splitting an expanded glob) — then the single consolidated
    // post-install tail (phase b). The dir flow passes the resolved
    // .PKGINFO name + version, so the tail targets the new <KVER>
    // directly (no local-DB derivation needed here).
    std::vector<InstallStep> steps{};
    std::string pacman_cmd = "pacman -U";
    for (const auto& pkg : pkgs) {
        // fs::absolute before quoting: list_local_packages hands out
        // the paths as found under `dir`, which may itself be relative.
        pacman_cmd += " '" + std::filesystem::absolute(pkg).string() + "'";
    }
    steps.push_back({pacman_cmd, true});
    append_postinstall_tail(steps, bl, name, version);

    // The install log (the real runner only — an injected test runner
    // records the raw commands and never touches the filesystem, so it
    // gets no log file: zero side effects, the k14 contract). Created
    // BEFORE the first step so the header (what is about to be
    // installed: source dir, package identities, bootloader) exists even
    // when the install fails. The known location:
    //   ~/.cache/kernel-manager/install-<YYYYMMDD-HHMMSS>.log
    std::string log_path{};
    if (use_default_runner) {
        const auto log_dir = utils::fix_path("~/.cache/kernel-manager");
        std::error_code ec{};
        std::filesystem::create_directories(log_dir, ec);
        const auto ts = format_log_times(std::time(nullptr));
        log_path = (std::filesystem::path{log_dir} / ("install-" + ts.file + ".log")).string();
        std::string header = "=== Kernel Manager Install Log ===\n"
                             "Date: " + ts.human + "\n"
                             "Source: " + std::string{dir} + "\n"
                             "Packages: " + package_identity(pkgs.front()) + "\n";
        for (std::size_t i = 1; i < pkgs.size(); ++i) {
            header += ", " + package_identity(pkgs[i]);
        }
        header += "\nBootloader: " + bootloader_name(bl) + "\n";
        utils::write_to_file(log_path, header);
        result.log_path = log_path;
    }

    // The execution loop (D-E, the 3-state verdict, with the install_logger
    // wrapper for the real runner): the phase-(a) pacman -U runs first; a
    // failure ⇒ INSTALLATION_FAILED and the tail never runs. The phase-(b)
    // consolidated tail (identified by its postinstall_tail.sh command, not
    // by position) is wrapped in install_logger.sh too, so its REAL rc (the
    // sentinel-captured exit code, chunk D) lands in the log AND drives the
    // verdict: 0 = BOOT_SAFE, != 0 = INSTALLED_NOT_BOOT_SAFE.
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto& step = steps[i];
        // Real runner: run the step through the installed
        // install_logger.sh — its output streams to the terminal in
        // real time AND is appended to the log (the command, the start
        // time, the command's REAL exit code, the full output). The
        // command stays a single quoted word: the tail step carries shell
        // syntax (the quoted package name / version / bootloader args)
        // that the script re-parses with `bash -c`, and runCmdTerminal's
        // appended `; read -p …` prompt lands AFTER the wrapper on the
        // terminal's command line.
        std::string cmd = step.cmd;
        if (use_default_runner) {
            const std::string label = "Step " + std::to_string(i + 1) + "/" + std::to_string(steps.size()) + ": " + short_label(cmd);
            cmd = KM_HELPER_DIR "/install_logger.sh " + shell_quote(log_path) + " " + shell_quote(label) + " " + shell_quote(cmd);
        }
        const int rc = runner(cmd, step.escalate);
        if (step.cmd.starts_with(KM_HELPER_DIR "/postinstall_tail.sh")) {
            // Phase (b): the tail's real rc is the verdict. Never a false
            // SUCCESS — the H3 fire-and-forget gap is closed by the sentinel.
            if (rc == 0) {
                result.verdict = PostinstallVerdict::BOOT_SAFE;
                result.ok = true;
            } else {
                result.verdict = PostinstallVerdict::INSTALLED_NOT_BOOT_SAFE;
                result.ok = false;
                result.error = "Post-install tail not boot-safe (exit code " + std::to_string(rc) + "): " + step.cmd;
            }
            if (use_default_runner) {
                append_log(log_path, "=== Result: " + verdict_label(result.verdict) + " ===");
            }
        } else if (rc != 0) {
            // Phase (a): the package install itself failed.
            result.verdict = PostinstallVerdict::INSTALLATION_FAILED;
            result.ok = false;
            result.error = "Command failed (exit code " + std::to_string(rc) + "): " + step.cmd;
            if (use_default_runner) {
                append_log(log_path, "=== Result: FAILED (step " + std::to_string(i + 1) + ": " + short_label(step.cmd)
                                 + ", exit code " + std::to_string(rc) + ") ===");
            }
            return result;  // the tail never runs; graceful, never a crash
        }
    }

    return result;
}
