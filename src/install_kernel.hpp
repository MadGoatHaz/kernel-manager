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

#ifndef INSTALL_KERNEL_HPP
#define INSTALL_KERNEL_HPP

#include "bootloader.hpp"      // for Bootloader, detect_bootloader
#include "known_kernels.hpp"   // for KnownKernel

#include <filesystem>  // for path
#include <functional>  // for function
#include <string>      // for string
#include <string_view> // for string_view
#include <vector>      // for vector

// The "install pre-compiled kernel" capability (plan chunk K8): plan the
// install steps for a kernel (pure, table-driven), execute them gracefully
// through the shared terminal path, and return the post-install
// boot-selection steps (bootloader detected, K5 + K6).
//
// Everything reuses the existing modules: the curated table
// (known_kernels), bootloader detection (bootloader), the per-bootloader
// instructions (boot_instructions), and the pkexec terminal path
// (utils::runCmdTerminal) — no logic from them is duplicated here.

// One command of an install plan: the shell command line and whether it
// needs root (an escalated step goes through the pkexec rootshell
// terminal path, utils::runCmdTerminal(cmd, true); an unescalated one
// runs as the user).
struct InstallStep {
    std::string cmd;
    bool escalate = true;
};

// Command executor with the utils::runCmdTerminal contract (0 on
// success, non-zero on failure). Injected into install_kernel() so unit
// tests record or fail steps without touching a real terminal, pkexec,
// pacman or the network.
using CommandRunner = std::function<int(const std::string& cmd, bool escalate)>;

// Pure install planning (no filesystem access, no commands run): the
// ordered step list that installs the given kernel on the given
// bootloader, derived only from the curated table data
// (known_kernels.hpp):
//   precompiled + pacman repo (core/extra/cachyos/chaotic-aur/liquorix):
//       `pacman -S --needed <package>` (escalated, phase a)
//       -> the single consolidated post-install tail (phase b): ONE
//          escalated `postinstall_tail.sh <pkg> <kver|empty> <bl>` step
//          (the script produced by chunks A/B/C) that, in ONE root
//          session, syncs the kernel drivers, regenerates the initramfs
//          for <kver>, verifies boot-safety and exits with the 3-state
//          verdict rc (0 = BOOT_SAFE, 1 = INSTALLED_NOT_BOOT_SAFE)
//   precompiled + AUR (install_repo == "aur"):
//       `paru -S --needed <package>` (not escalated — an AUR build runs
//       as the user) -> the same single tail step
//   buildable only (no precompiled package):
//       `KM_HELPER_DIR "/build_helper.sh -sicf --cleanbuild"` (not
//       escalated — the makepkg wrapper auto-imports a missing GPG key
//       on "unknown public key" and retries; the source repo is prepared
//       by the existing build environment — see detail::install_aur_kernels
//       in aur_kernel.cpp and the Build-custom (Configure) flow)
//   neither: an empty plan.
[[gnu::pure]] [[nodiscard]] std::vector<InstallStep> plan_steps(const KnownKernel& kernel, Bootloader bl);

// The honest 3-state result of a precompiled install (D-E, R4): computed
// from the TWO real exit codes the terminal-helper sentinel (chunk D)
// captures — the phase-(a) package-install rc and the phase-(b)
// consolidated-tail (postinstall_tail.sh, chunks A/B/C) rc. Never a
// hard-coded SUCCESS (the old all-`|| true` + unconditional "SUCCESS"
// verification, H3, is gone).
//   BOOT_SAFE               — phase a rc 0 AND the tail rc 0: installed and
//                             confirmed boot-safe (driver modules + a
//                             non-empty per-<KVER> initramfs + a boot entry).
//   INSTALLED_NOT_BOOT_SAFE — phase a rc 0 but the tail rc != 0: the package
//                             is installed but a tail stage left a gap
//                             (a DISTINCT, honest outcome, never masked as ok).
//   INSTALLATION_FAILED     — the phase-(a) package install itself failed;
//                             the tail never runs.
enum class PostinstallVerdict { BOOT_SAFE, INSTALLED_NOT_BOOT_SAFE, INSTALLATION_FAILED };

// Result of one install_kernel() run: the outcome flag + error text, the
// 3-state post-install verdict, and the post-install boot-selection steps.
// `ok` is true iff the verdict is BOOT_SAFE (phase a AND the tail both rc
// 0). The instructions are filled on every run (they describe the detected
// bootloader and the kernel, not the outcome), so the caller can show them
// even after a failure.
struct InstallKernelResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> boot_instructions;
    PostinstallVerdict verdict = PostinstallVerdict::INSTALLATION_FAILED;
};

// Executes the plan for `kernel` (plan_steps above): runs each step
// through `runner` in order and stops at the first failing step (ok =
// false, `error` names the failed command and its exit code, the
// remaining steps are skipped — graceful, never a crash).
//   - an empty runner selects the real one (utils::runCmdTerminal, the
//     shared pkexec terminal path; resolved in the .cpp so this header
//     stays Qt-free)
//   - with the real runner, a precompiled AUR install without `paru`
//     falls back to the existing AUR build path (detail::
//     install_aur_kernels) instead of failing — the same pattern
//     kernel-manager already uses for AUR kernels
//   - a buildable-only kernel (no precompiled package) fails gracefully
//     with a message pointing at the Build-custom (Configure) flow and
//     runs no command
// `bl` is the bootloader the post-install tail targets (default: the
// live detection, detect_bootloader()).
[[nodiscard]] InstallKernelResult install_kernel(const KnownKernel& kernel,
                                                 CommandRunner runner = CommandRunner{},
                                                 Bootloader bl = detect_bootloader());

// Name-based convenience API for the "install pre-compiled <kernel>"
// action. The curated table lookup is prefix-tolerant
// ("core/linux" -> "linux"); an unknown name takes the K1 fallback
// (its own package as the build source, no precompiled path, never
// unbuildable).
struct InstallPlan {
    std::string kernel;  // the kernel name (repo prefix stripped)
    std::string package; // pre-compiled package name ("" if none)
    std::string repo;    // pacman repo name ("aur" when AUR; "" when unknown)
    bool precompiled = false;
    std::vector<std::string> install_cmds;      // the package install step(s) (phase a)
    std::vector<std::string> postinstall_cmds;  // the single consolidated post-install tail (phase b): one postinstall_tail.sh step (driver sync + initramfs for <KVER> + boot-safety verify, chunks A/B/C) carrying the 3-state verdict rc
    std::string note;                           // build-only guidance ("" when precompiled)
};

// The install plan for a kernel name: the table lookup + plan_steps,
// split into the package install commands and the post-install tail
// (computed for the live-detected bootloader). A build-only (or
// unknown) kernel gets empty command lists and a note pointing at the
// Build-custom (Configure) flow.
[[nodiscard]] InstallPlan plan_install(std::string_view kernel_name);

// Executes a plan: the install commands first, then the post-install
// tail, each through `runner` (an empty runner selects the real
// utils::runCmdTerminal, escalated). Graceful: the first failing
// command lands in `error_out` and false is returned, the remaining
// commands are skipped, no crash. A build-only plan (precompiled =
// false) returns false with the note and runs nothing.
[[nodiscard]] bool execute_plan(const InstallPlan& plan,
                                std::string& error_out,
                                CommandRunner runner = CommandRunner{});

// The post-install boot-selection steps for a kernel name: the live
// bootloader detection (K5) + instructions_for (K6) with the kernel's
// package base — the thin K5+K6 combination the "show boot
// instructions" action displays.
[[nodiscard]] std::vector<std::string> boot_instructions_for(std::string_view kernel_name);

// ── Install from a directory (Feature A, D2) ─────────────────────────
//
// The "Install from directory…" action's backend: the locally built
// package(s) found in a user-picked directory are installed with one
// escalated `pacman -U` of explicit absolute paths + the standard
// post-install tail — the same CommandRunner contract and graceful-stop
// execution as install_kernel() above, but driven by the directory
// contents instead of the curated table (the file set is unknown at
// plan time, so there is no plan_steps analog: the plan is built at
// execution time from list_local_packages).

// List the built packages in a directory (non-recursive: a makepkg
// build dir holds them at its root, the post-build flow's layout): the
// *.pkg.tar.zst files, sorted by filename for a deterministic order.
// The literal .pkg.tar.zst suffix is the current contract (user
// request); a PKGEXT-from-/etc/makepkg.conf generalization (the
// get_pkgext_value_from_makepkgconf precedent) is documented future
// work, not this cycle.
[[nodiscard]] std::vector<std::filesystem::path> list_local_packages(std::string_view dir);

// Read the package identity out of a built package's .PKGINFO
// (extracted in place with `tar --zstd -xOf` — the archive is never
// modified): `name` = the pkgname field, `version` =
// "<pkgver>-<pkgrel>" (a missing pkgrel degrades to the bare pkgver).
// False on any failure (non-archive, missing or unreadable .PKGINFO,
// absent pkgname) — the caller falls back to the filename; the install
// itself never depends on this parse.
[[nodiscard]] bool read_pkginfo(const std::filesystem::path& pkg,
                                std::string& name_out,
                                std::string& version_out);

// Result of one install_from_directory() run: the outcome flag + error
// text, the 3-state post-install verdict (D-E), the installed kernel's
// name + version (for the boot-instructions display and the list refresh),
// the post-install boot-selection steps, and the install log's path. `ok`
// is true iff the verdict is BOOT_SAFE. The instructions are filled on
// every run (they describe the detected bootloader and the kernel, not the
// outcome), so the caller can show them even after a failure — except when
// the directory holds no packages at all (zero commands, no kernel to point
// at). `log_path` is filled only for the real (default) runner: the log
// holds every executed step's command, real exit code and full output plus
// the tail's VERDICT line (see install_from_directory below); an injected
// test runner executes nothing, so it gets an empty `log_path` (zero
// filesystem side effects).
struct DirInstallResult {
    bool ok = false;
    std::string error;
    std::string name;
    std::string version;
    std::vector<std::string> boot_instructions;
    std::string log_path;  // the install log ("" for an injected runner)
    PostinstallVerdict verdict = PostinstallVerdict::INSTALLATION_FAILED;
};

// Install the built packages found in `dir` (list_local_packages): one
// escalated `pacman -U` of the explicit single-quoted ABSOLUTE paths
// (no shell glob — the 0c918d4 pkexec-CWD-reset rationale: the root
// shell starts in $HOME, so relative paths break, and quoting keeps a
// spaced name one word instead of word-splitting an expanded glob)
// followed by the single consolidated post-install tail (one escalated
// postinstall_tail.sh step: driver sync + the initramfs for the new
// <KVER> + the boot-safety verify, chunks A/B/C), each step through
// `runner`:
//   - an empty runner selects the real one (utils::runCmdTerminal, the
//     shared pkexec terminal path; resolved in the .cpp so this header
//     stays Qt-free)
//   - a directory with no *.pkg.tar.zst packages fails with the
//     "no *.pkg.tar.zst packages found in '<dir>'" error and runs zero
//     commands (the D2 guard)
//   - the first failing step lands in `error` (the command + its exit
//     code), the remaining steps are skipped — graceful, never a crash
// `name`/`version` come from the first package whose .PKGINFO parses
// to a non-headers kernel name (read_pkginfo), else the first filename
// (best-effort: the .pkg.tar.zst suffix stripped); `bl` is the
// bootloader the post-install tail targets (default: the live
// detection, detect_bootloader()).
//
// Install logging (real runner only — an injected runner records the
// raw commands and never touches the filesystem): the run writes a log
// at ~/.cache/kernel-manager/install-<YYYYMMDD-HHMMSS>.log (created
// before the first step, the path returned in `log_path`): a header
// (date, source dir, package identities, bootloader) + one section per
// executed step (the command, start time, the command's REAL exit code
// and its full stdout+stderr — each step runs through the installed
// install_logger.sh helper, which streams the output to the terminal in
// real time AND appends it to the log) + a result tail (the verdict:
// BOOT_SAFE, INSTALLED_NOT_BOOT_SAFE, or FAILED naming the first failing
// step). The consolidated tail's own verify stage (the driver modules, the
// per-<KVER> initramfs, the boot entry) is what the log's boot-safety
// record is — the separate read-only verification compound command is gone,
// the tail IS the verification now. The log is the post-mortem record: the
// terminal windows are transient and their output is otherwise lost.
[[nodiscard]] DirInstallResult install_from_directory(std::string_view dir,
                                                      CommandRunner runner = CommandRunner{},
                                                      Bootloader bl = detect_bootloader());

#endif  // INSTALL_KERNEL_HPP
