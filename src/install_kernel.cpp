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
#include "utils.hpp"             // for runCmdTerminal (default runner), exec (paru probe)

#include <string>      // for string, to_string
#include <string_view> // for string_view
#include <utility>     // for move
#include <vector>      // for vector

namespace {

// The real command runner: the shared terminal path (an escalated
// command is wrapped in pkexec rootshell — see utils.cpp).
int run_real_command(const std::string& cmd, bool escalate) noexcept {
    return utils::runCmdTerminal(QString::fromStdString(cmd), escalate);
}

// A step that installs a package (pacman/paru/makepkg), as opposed to
// the post-install tail (mkinitcpio / grub-mkconfig).
[[gnu::pure]] bool is_package_install_step(const std::string& cmd) {
    return cmd.starts_with("pacman ") || cmd.starts_with("paru ") || cmd.starts_with("makepkg ");
}

// The post-install tail every precompiled install gets: the initramfs
// refresh, then the GRUB config regeneration (GRUB only —
// systemd-boot/UKI auto-detect the new kernel, UNKNOWN needs nothing
// beyond the initramfs refresh).
void append_postinstall_tail(std::vector<InstallStep>& steps, Bootloader bl) {
    steps.push_back({"mkinitcpio -P", true});
    if (bl == Bootloader::GRUB) {
        steps.push_back({"grub-mkconfig -o /boot/grub/grub.cfg", true});
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
        append_postinstall_tail(steps, bl);
        return steps;
    }

    if (kernel.buildable && !kernel.default_source.empty()) {
        // No precompiled package: the custom build path — the same
        // makepkg command the app already runs for AUR kernels (the
        // source repo is prepared by the existing build environment,
        // see detail::install_aur_kernels / the Configure flow).
        steps.push_back({"makepkg -sicf --cleanbuild", false});
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

    for (const auto& step : steps) {
        const int rc = runner(step.cmd, step.escalate);
        if (rc != 0) {
            result.ok = false;
            result.error = "Command failed (exit code " + std::to_string(rc) + "): " + step.cmd;
            return result;  // skip the remaining steps; graceful, never a crash
        }
    }

    result.ok = true;
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
