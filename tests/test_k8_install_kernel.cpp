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

// Standalone unit test for the K8 install-kernel module (no CTest infra
// in this project; follows the "standalone g++ on the real sources"
// precedent of tests/run_k6.sh). Compiles the real module plus the
// modules it reuses (known_kernels, bootloader, boot_instructions,
// aur_kernel, utils) with the project's GCC warning set and asserts:
//   - plan_steps (pure, table-driven): core pacman precompiled (exact
//     2-step sequence — the package install + the single consolidated
//     postinstall_tail.sh step (chunk E), per-bootloader tail args,
//     escalation flags), the AUR branch (non-escalated `paru -S --needed`
//     + the same single tail), buildable-only (single non-escalated
//     build-helper step — makepkg with auto GPG import —, mirroring
//     aur_kernel.cpp), synthetic entries (environment-independent)
//   - the real-table cases (linux always; linux-xanmod + linux-tkg
//     gated on the K3 community rows so the harness passes on a
//     13-row pre-K3 main too): xanmod is precompiled from the
//     chaotic-aur *pacman* repo (not AUR) per the curated table, so it
//     plans `pacman -S --needed linux-xanmod`, not a paru/makepkg path
//   - plan_install (the name-based convenience API): table fields,
//     prefix tolerance ("core/linux"), unknown-name fallback (no
//     commands + the Build-custom note), build-only (no commands +
//     note)
//   - install_kernel with a recording CommandRunner: the exact
//     cmd/escalate sequence on success (package install + the single
//     tail); the 3-state verdict (D-E) from the REAL rcs — phase-a
//     rc!=0 => INSTALLATION_FAILED (the tail is skipped); tail rc 0 =>
//     BOOT_SAFE (ok); tail rc 1 or 2 => INSTALLED_NOT_BOOT_SAFE (ok
//     false, never a false SUCCESS); buildable-only => graceful note,
//     zero commands run
//   - execute_plan: build-only => false + note (real runner selected
//     but never used — nothing is executed); failing injected runner =>
//     false + error + one command attempted; ok injected runner => true
//     + exactly the planned commands
//   - boot_instructions_for: non-empty, ends with the mandatory
//     ready-to-boot note, prefix tolerant, live bootloader detection
//
// No real terminal, pkexec, pacman, paru or network is touched: every
// executed command goes through an injected recording runner.

#include "install_kernel.hpp"

#include <cstdio>   // for printf
#include <string>   // for string
#include <utility>  // for pair
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

// True when any command in the list STARTS with the prefix (a
// substring match would be wrong: the guarded initramfs step contains
// "mkinitcpio" as one of its `command -v` sub-actions, so only a
// prefix test can tell a standalone mkinitcpio step from the guarded
// sub-action). Used to assert no standalone mkinitcpio / dkms step is
// present and that no GRUB refresh step is on the non-GRUB bootloaders.
bool any_starts_with(const std::vector<std::string>& cmds, const std::string& prefix) {
    for (const auto& cmd : cmds) {
        if (cmd.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

// The single consolidated tail step append_postinstall_tail emits (chunk E;
// the exact command the .cpp pushes, kept here so the assertions stay in
// lockstep with the implementation): ONE postinstall_tail.sh call (chunks
// A/B/C) carrying (pkgname, kver, bootloader). The bootloader name follows
// the tail's `case "$BL"` contract (chunk C) — the lowercase systemd-boot /
// grub / uki / unknown — NOT the UI-facing bootloader_name() strings
// ("GRUB", "Unified Kernel Image (UKI)", "Unknown") that would fall into the
// tail's out-of-contract default. `kver` is the .PKGINFO version for a dir
// install and the EMPTY string for the repo/AUR flow (the tail then derives
// it from the local DB). The single quotes mirror the .cpp's shell_quote for
// these quote-free values.
std::string tail_cmd(const std::string& pkg, const std::string& kver, Bootloader bl) {
    const char* blname = "unknown";
    switch (bl) {
        case Bootloader::SYSTEMD_BOOT: blname = "systemd-boot"; break;
        case Bootloader::GRUB:         blname = "grub"; break;
        case Bootloader::UKI:          blname = "uki"; break;
        case Bootloader::UNKNOWN:      blname = "unknown"; break;
    }
    return std::string(KM_HELPER_DIR) + "/postinstall_tail.sh '" + pkg + "' '" + kver + "' '" + blname + "'";
}

// The mandatory closing note: one constant text instructions_for (K6)
// appends for every bootloader (the kernel is installed and ready to
// boot; select it from the bootloader menu at next reboot — no tool
// command, no /boot/ or /efi/ loader path is named).
constexpr const char* expected_note =
    "The kernel is installed and ready to boot. Select it from your bootloader menu at next reboot.";

// A recording command runner: returns a fixed exit code for every step
// and records the (cmd, escalate) pairs asked to run, so no real
// terminal, pkexec, pacman or network is ever touched.
struct RecordingRunner {
    int rc = 0;
    std::vector<std::pair<std::string, bool>> calls{};
    CommandRunner fn = [this](const std::string& cmd, bool escalate) noexcept -> int {
        calls.emplace_back(cmd, escalate);
        return rc;
    };
};

// A per-call scripted runner (the D-E 3-state verdict probe): returns a
// distinct exit code for the Nth call (rcs[0] for call 1, rcs[1] for call 2,
// ...; 0 past the end) and records the (cmd, escalate) pairs, so a phase-a
// rc 0 + tail rc 1 can be driven to INSTALLED_NOT_BOOT_SAFE (and a phase-a
// rc 7 to INSTALLATION_FAILED) — no real terminal, pkexec, pacman or network
// is touched.
struct ScriptedRunner {
    std::vector<int> rcs{};
    std::vector<std::pair<std::string, bool>> calls{};
    CommandRunner fn = [this](const std::string& cmd, bool escalate) noexcept -> int {
        calls.emplace_back(cmd, escalate);
        const std::size_t idx = calls.size() - 1;
        return idx < rcs.size() ? rcs[idx] : 0;
    };
};

}  // namespace

int main() {
    // ------------------------------------------------------------------
    // 1. plan_steps: core pacman precompiled, per bootloader.
    //    ("linux" is curated since K1 and is always in the table.)
    // ------------------------------------------------------------------
    // find_kernel hands out optional<const KnownKernel*>; the "linux"
    // row is curated since K1 and is always in the table.
    const auto maybe_linux = km::find_kernel("linux");
    check(maybe_linux.has_value(), "table: 'linux' is curated");
    const KnownKernel* const linux = maybe_linux.has_value() ? *maybe_linux : nullptr;
    if (linux != nullptr) {
        const std::vector<InstallStep> grub = plan_steps(*linux, Bootloader::GRUB);
        check(grub.size() == 2, "linux+GRUB: exactly 2 steps (package install + the single consolidated tail)");
        if (grub.size() == 2) {
            check(grub[0].cmd == "pacman -S --needed linux" && grub[0].escalate,
                  "linux+GRUB[0]: escalated `pacman -S --needed linux`");
            check(grub[1].cmd == tail_cmd("linux", "", Bootloader::GRUB) && grub[1].escalate,
                  "linux+GRUB[1]: the single consolidated tail (escalated; the GRUB refresh now lives inside it)");
        }

        for (const Bootloader bl : {Bootloader::SYSTEMD_BOOT, Bootloader::UKI, Bootloader::UNKNOWN}) {
            const std::string label = bootloader_name(bl);
            const std::vector<InstallStep> steps = plan_steps(*linux, bl);
            check(steps.size() == 2,
                  (label + ": exactly 2 steps (install + the single consolidated tail)").c_str());
            if (steps.size() == 2) {
                check(steps[0].cmd == "pacman -S --needed linux" && steps[0].escalate,
                      (label + "[0]: pacman install").c_str());
                check(steps[1].cmd == tail_cmd("linux", "", bl) && steps[1].escalate,
                      (label + "[1]: the single consolidated tail (escalated, per-bootloader arg = the tail's name)").c_str());
            }
        }
    }

    // ------------------------------------------------------------------
    // 2. plan_steps: the AUR branch (synthetic entry — the curated
    //    table documents no precompiled AUR row, but the branch must
    //    exist): non-escalated `paru -S --needed`, same tail.
    // ------------------------------------------------------------------
    {
        const KnownKernel aur{
            "linux-aurx",     // name
            "AUR Test",       // display_name
            "A synthetic precompiled AUR package.",  // description
            "linux-aurx",     // default_source
            "linux-aurx",     // install_package
            "aur",            // install_repo
            true,             // precompiled_available
            true              // buildable
        };
        const std::vector<InstallStep> uki = plan_steps(aur, Bootloader::UKI);
        check(uki.size() == 2, "AUR+UKI: exactly 2 steps (install + the single consolidated tail)");
        if (uki.size() == 2) {
            check(uki[0].cmd == "paru -S --needed linux-aurx" && !uki[0].escalate,
                  "AUR+UKI[0]: non-escalated `paru -S --needed`");
            check(uki[1].cmd == tail_cmd("linux-aurx", "", Bootloader::UKI) && uki[1].escalate,
                  "AUR+UKI[1]: the single consolidated tail (escalated)");
        }
        const std::vector<InstallStep> grub = plan_steps(aur, Bootloader::GRUB);
        check(grub.size() == 2, "AUR+GRUB: exactly 2 steps (install + the single consolidated tail)");
        if (grub.size() == 2) {
            check(grub[0].cmd == "paru -S --needed linux-aurx" && !grub[0].escalate, "AUR+GRUB[0]: paru non-escalated");
            check(grub[1].cmd == tail_cmd("linux-aurx", "", Bootloader::GRUB) && grub[1].escalate,
                  "AUR+GRUB[1]: the single consolidated tail (escalated)");
        }
    }

    // ------------------------------------------------------------------
    // 3. plan_steps: buildable-only (synthetic tkg-like entry — no
    //    precompiled package): the single non-escalated makepkg step,
    //    mirroring detail::install_aur_kernels (aur_kernel.cpp).
    // ------------------------------------------------------------------
    {
        const KnownKernel tkg{
            "linux-tkg",     // name
            "TKG Test",      // display_name
            "A synthetic build-only kernel.",  // description
            "https://github.com/Frogging-Family/linux-tkg.git",  // default_source
            "",              // install_package (none)
            "chaotic-aur",   // install_repo
            false,           // precompiled_available
            true             // buildable
        };
        const std::vector<InstallStep> steps = plan_steps(tkg, Bootloader::UNKNOWN);
        check(steps.size() == 1, "build-only: exactly 1 step");
        if (steps.size() == 1) {
            check(steps[0].cmd == KM_HELPER_DIR "/build_helper.sh -sicf --cleanbuild" && !steps[0].escalate,
                  "build-only[0]: non-escalated build helper (makepkg + auto GPG import)");
        }
    }

    // ------------------------------------------------------------------
    // 4. Real-table spot checks (gated: linux-xanmod + linux-tkg exist
    //    only on a main that already merged K3's community rows; on a
    //    13-row pre-K3 main the cases are skipped with an INFO line, so
    //    the harness passes on both).
    // ------------------------------------------------------------------
    if (const auto found = km::find_kernel("linux-xanmod"); found.has_value()) {
        const KnownKernel* const xanmod = *found;
        check(xanmod->precompiled_available && xanmod->install_repo == "chaotic-aur",
              "xanmod: precompiled from the chaotic-aur pacman repo (curated table)");
        const std::vector<InstallStep> steps = plan_steps(*xanmod, Bootloader::SYSTEMD_BOOT);
        check(steps.size() == 2, "xanmod+systemd-boot: exactly 2 steps (install + the single consolidated tail)");
        if (steps.size() == 2) {
            check(steps[0].cmd == "pacman -S --needed linux-xanmod" && steps[0].escalate,
                  "xanmod[0]: escalated `pacman -S --needed linux-xanmod` (chaotic-aur is pacman, not AUR)");
            check(steps[1].cmd == tail_cmd("linux-xanmod", "", Bootloader::SYSTEMD_BOOT) && steps[1].escalate,
                  "xanmod+systemd-boot: the single consolidated tail (no separate GRUB refresh)");
        }
    } else {
        std::printf("INFO: linux-xanmod not curated on this table (pre-K3 main) - AUR/pacman branch covered by the synthetic entries\n");
    }
    if (const auto found = km::find_kernel("linux-tkg"); found.has_value()) {
        const KnownKernel* const tkg = *found;
        check(!tkg->precompiled_available && tkg->install_package.empty(), "tkg: build-only, no precompiled package");
        const std::vector<InstallStep> steps = plan_steps(*tkg, Bootloader::UNKNOWN);
        check(steps.size() == 1 && steps[0].cmd == KM_HELPER_DIR "/build_helper.sh -sicf --cleanbuild" && !steps[0].escalate,
              "tkg: the build-helper makepkg path, non-escalated");
    } else {
        std::printf("INFO: linux-tkg not curated on this table (pre-K3 main) - build-only branch covered by the synthetic entry\n");
    }

    // ------------------------------------------------------------------
    // 5. plan_install (the name-based convenience API).
    // ------------------------------------------------------------------
    {
        const InstallPlan plan = plan_install("linux");
        check(plan.kernel == "linux" && plan.package == "linux" && plan.repo == "core",
              "plan_install(linux): kernel/package/repo from the table");
        check(plan.precompiled, "plan_install(linux): precompiled");
        check(plan.note.empty(), "plan_install(linux): no build-only note");
        check(plan.install_cmds.size() == 1 && plan.install_cmds[0] == "pacman -S --needed linux",
              "plan_install(linux): install_cmds = [`pacman -S --needed linux`]");
        // The postinstall is now the SINGLE consolidated tail (chunk E): one
        // postinstall_tail.sh step carrying the live-detected bootloader,
        // with an empty kver (the repo/AUR flow — the tail derives it from
        // the local DB). No separate driver / initramfs / GRUB refresh
        // commands remain in the C++ (they all live inside the script).
        check(plan.postinstall_cmds.size() == 1
              && plan.postinstall_cmds[0] == tail_cmd("linux", "", detect_bootloader()),
              "plan_install(linux): postinstall = the single consolidated tail (live detect)");
        // The single tail call is the only postinstall step, and no inline
        // driver / initramfs / GRUB command (the old sh -c / dkms /
        // mkinitcpio / dracut / grub-mkconfig steps) may remain in the C++.
        check(!any_starts_with(plan.postinstall_cmds, "sh -c")
              && !any_starts_with(plan.postinstall_cmds, "dkms")
              && !any_starts_with(plan.postinstall_cmds, "mkinitcpio")
              && !any_starts_with(plan.postinstall_cmds, "dracut")
              && !any_starts_with(plan.postinstall_cmds, "grub-mkconfig"),
              "plan_install(linux): no inline driver / initramfs / GRUB command (command -v-free in the C++)");
        std::printf("INFO: plan_install(linux) -> %zu install + %zu postinstall cmd(s), live bootloader = %s\n",
                    plan.install_cmds.size(), plan.postinstall_cmds.size(), bootloader_name(detect_bootloader()).c_str());
    }
    {
        const InstallPlan prefix = plan_install("core/linux");
        check(prefix.kernel == "linux" && prefix.install_cmds == plan_install("linux").install_cmds,
              "plan_install(core/linux): prefix-tolerant, same install commands");
    }
    {
        const InstallPlan unknown = plan_install("linux-mystery-xyz");
        check(unknown.kernel == "linux-mystery-xyz" && !unknown.precompiled,
              "plan_install(unknown): K1 fallback, not precompiled");
        check(unknown.install_cmds.empty() && unknown.postinstall_cmds.empty(),
              "plan_install(unknown): no commands planned");
        check(contains(unknown.note, "Build-custom"), "plan_install(unknown): note points at the Build-custom flow");
    }
    if (km::find_kernel("linux-tkg").has_value()) {
        const InstallPlan tkg = plan_install("linux-tkg");
        check(!tkg.precompiled && tkg.install_cmds.empty() && tkg.postinstall_cmds.empty(),
              "plan_install(tkg): build-only, no commands");
        check(contains(tkg.note, "Build-custom"), "plan_install(tkg): note points at the Build-custom flow");
    }
    if (km::find_kernel("linux-xanmod").has_value()) {
        const InstallPlan xanmod = plan_install("linux-xanmod");
        check(xanmod.precompiled && xanmod.repo == "chaotic-aur", "plan_install(xanmod): precompiled, chaotic-aur repo");
        check(xanmod.install_cmds.size() == 1 && xanmod.install_cmds[0] == "pacman -S --needed linux-xanmod",
              "plan_install(xanmod): pacman install (chaotic-aur is a pacman repo)");
    }

    // ------------------------------------------------------------------
    // 6. install_kernel with a recording CommandRunner (plan's exact
    //    sequence contract; nothing real is executed).
    // ------------------------------------------------------------------
    if (linux != nullptr) {
        RecordingRunner ok_runner{};
        const InstallKernelResult ok_result = install_kernel(*linux, ok_runner.fn, Bootloader::GRUB);
        check(ok_result.ok && ok_result.error.empty(), "install_kernel(linux, GRUB, ok): ok, no error");
        check(ok_result.verdict == PostinstallVerdict::BOOT_SAFE, "install_kernel(linux, GRUB, ok): verdict == BOOT_SAFE");
        check(ok_runner.calls.size() == 2, "install_kernel: exactly 2 commands run (package install + the single tail), in order");
        if (ok_runner.calls.size() == 2) {
            check(ok_runner.calls[0].first == "pacman -S --needed linux" && ok_runner.calls[0].second,
                  "install_kernel[0]: escalated pacman");
            check(ok_runner.calls[1].first == tail_cmd("linux", "", Bootloader::GRUB) && ok_runner.calls[1].second,
                  "install_kernel[1]: the single consolidated tail (escalated)");
        }
        check(!ok_result.boot_instructions.empty() && ok_result.boot_instructions.back() == expected_note,
              "install_kernel: boot instructions filled, ends with the note");

        RecordingRunner fail_runner{};
        fail_runner.rc = 1;
        const InstallKernelResult fail_result = install_kernel(*linux, fail_runner.fn, Bootloader::GRUB);
        check(!fail_result.ok, "install_kernel(failing phase-a runner): not ok");
        check(fail_result.verdict == PostinstallVerdict::INSTALLATION_FAILED,
              "install_kernel(failing phase-a): verdict == INSTALLATION_FAILED");
        check(contains(fail_result.error, "pacman") && contains(fail_result.error, "1"),
              "install_kernel: error names the failed command and its exit code");
        check(fail_runner.calls.size() == 1, "install_kernel: post-install tail skipped after a phase-a failure");
        check(!fail_result.boot_instructions.empty() && fail_result.boot_instructions.back() == expected_note,
              "install_kernel: boot instructions still filled on failure");

        // The D-E 3-state verdict probe (per-call rcs — the phase-a rc and the
        // tail rc are judged independently; the H3 false-SUCCESS is gone):
        //   phase-a rc 0 + tail rc 1  => INSTALLED_NOT_BOOT_SAFE (ok false)
        //   phase-a rc 0 + tail rc 2  => INSTALLED_NOT_BOOT_SAFE (raw rc)
        //   phase-a rc 0 + tail rc 0  => BOOT_SAFE (ok true)
        //   phase-a rc 7 (+ tail rc 0)=> INSTALLATION_FAILED (tail skipped)
        ScriptedRunner not_boot_safe{};
        not_boot_safe.rcs = {0, 1};
        const InstallKernelResult r_nbs = install_kernel(*linux, not_boot_safe.fn, Bootloader::GRUB);
        check(!r_nbs.ok && r_nbs.verdict == PostinstallVerdict::INSTALLED_NOT_BOOT_SAFE,
              "install_kernel(tail rc=1): INSTALLED_NOT_BOOT_SAFE, not a false SUCCESS");
        check(contains(r_nbs.error, "not boot-safe") && contains(r_nbs.error, "1"),
              "install_kernel(tail rc=1): the error names the tail + its raw rc");

        ScriptedRunner tail_rc2{};
        tail_rc2.rcs = {0, 2};
        const InstallKernelResult r_rc2 = install_kernel(*linux, tail_rc2.fn, Bootloader::GRUB);
        check(!r_rc2.ok && r_rc2.verdict == PostinstallVerdict::INSTALLED_NOT_BOOT_SAFE && contains(r_rc2.error, "2"),
              "install_kernel(tail rc=2): INSTALLED_NOT_BOOT_SAFE with the raw rc (>=2 folds in)");

        ScriptedRunner all_zero{};
        all_zero.rcs = {0, 0};
        const InstallKernelResult r_bs = install_kernel(*linux, all_zero.fn, Bootloader::GRUB);
        check(r_bs.ok && r_bs.verdict == PostinstallVerdict::BOOT_SAFE,
              "install_kernel(phase-a rc=0 + tail rc=0): BOOT_SAFE, ok");

        ScriptedRunner phase_a_fail{};
        phase_a_fail.rcs = {7};
        const InstallKernelResult r_if = install_kernel(*linux, phase_a_fail.fn, Bootloader::GRUB);
        check(!r_if.ok && r_if.verdict == PostinstallVerdict::INSTALLATION_FAILED,
              "install_kernel(phase-a rc=7): INSTALLATION_FAILED");
        check(phase_a_fail.calls.size() == 1, "install_kernel(phase-a rc=7): the tail is skipped");
    }

    {
        // AUR branch, injected runner: the exact paru sequence.
        const KnownKernel aur{
            "linux-aurx",     // name
            "AUR Test",       // display_name
            "A synthetic precompiled AUR package.",  // description
            "linux-aurx",     // default_source
            "linux-aurx",     // install_package
            "aur",            // install_repo
            true,             // precompiled_available
            true              // buildable
        };
        RecordingRunner runner{};
        const InstallKernelResult result = install_kernel(aur, runner.fn, Bootloader::GRUB);
        check(result.ok && result.verdict == PostinstallVerdict::BOOT_SAFE,
              "install_kernel(AUR, ok runner): ok, verdict == BOOT_SAFE");
        check(runner.calls.size() == 2, "install_kernel(AUR): exactly 2 commands (paru + the single tail)");
        if (runner.calls.size() == 2) {
            check(runner.calls[0].first == "paru -S --needed linux-aurx" && !runner.calls[0].second,
                  "install_kernel(AUR)[0]: non-escalated paru");
            check(runner.calls[1].first == tail_cmd("linux-aurx", "", Bootloader::GRUB) && runner.calls[1].second,
                  "install_kernel(AUR)[1]: the single consolidated tail (escalated)");
        }
    }

    {
        // Buildable-only: graceful note, zero commands run.
        const KnownKernel tkg{
            "linux-tkg",     // name
            "TKG Test",      // display_name
            "A synthetic build-only kernel.",  // description
            "https://github.com/Frogging-Family/linux-tkg.git",  // default_source
            "",              // install_package (none)
            "chaotic-aur",   // install_repo
            false,           // precompiled_available
            true             // buildable
        };
        RecordingRunner runner{};
        const InstallKernelResult result = install_kernel(tkg, runner.fn, Bootloader::UNKNOWN);
        check(!result.ok && contains(result.error, "Build-custom"),
              "install_kernel(build-only): graceful Build-custom note");
        check(result.verdict == PostinstallVerdict::INSTALLATION_FAILED,
              "install_kernel(build-only): degenerate default verdict == INSTALLATION_FAILED (no precompiled install ran)");
        check(runner.calls.empty(), "install_kernel(build-only): no command run");
        check(!result.boot_instructions.empty() && result.boot_instructions.back() == expected_note,
              "install_kernel(build-only): boot instructions filled (the constant note)");
    }

    // ------------------------------------------------------------------
    // 7. execute_plan (the InstallPlan convenience API).
    // ------------------------------------------------------------------
    {
        // Build-only plan (unknown kernel — always available): the real
        // runner is selected by the 2-arg form but never used; nothing
        // is executed in any environment.
        std::string error{};
        const InstallPlan plan = plan_install("linux-mystery-xyz");
        check(!execute_plan(plan, error), "execute_plan(build-only): false");
        check(contains(error, "Build-custom"), "execute_plan(build-only): error carries the note");
    }
    {
        // Precompiled plan, failing injected runner: false + error,
        // exactly one command attempted (the tail is skipped).
        const InstallPlan plan = plan_install("linux");
        RecordingRunner fail_runner{};
        fail_runner.rc = 1;
        std::string error{};
        check(!execute_plan(plan, error, fail_runner.fn), "execute_plan(failing runner): false");
        check(!error.empty() && contains(error, "pacman"), "execute_plan(failing runner): error names the command");
        check(fail_runner.calls.size() == 1, "execute_plan(failing runner): no post-install after the failure");
    }
    {
        // Precompiled plan, ok injected runner: true, no error, exactly
        // the planned commands run (install first, then the tail).
        const InstallPlan plan = plan_install("linux");
        RecordingRunner ok_runner{};
        std::string error{};
        check(execute_plan(plan, error, ok_runner.fn), "execute_plan(ok runner): true");
        check(error.empty(), "execute_plan(ok runner): no error");
        const std::size_t expected = plan.install_cmds.size() + plan.postinstall_cmds.size();
        check(ok_runner.calls.size() == expected, "execute_plan: runs exactly the planned commands");
    }

    // ------------------------------------------------------------------
    // 8. boot_instructions_for (the thin K5+K6 combination).
    // ------------------------------------------------------------------
    {
        const std::vector<std::string> steps = boot_instructions_for("linux");
        check(!steps.empty(), "boot_instructions_for(linux): non-empty");
        if (!steps.empty()) {
            check(steps.back() == expected_note, "boot_instructions_for(linux): ends with the ready-to-boot note");
        }
        check(boot_instructions_for("core/linux") == steps,
              "boot_instructions_for(core/linux): prefix-tolerant, identical to linux");
        std::printf("INFO: live detection -> %s (%zu boot-instruction step(s))\n",
                    bootloader_name(detect_bootloader()).c_str(), steps.size());
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
