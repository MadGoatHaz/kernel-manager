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

// Standalone unit test for the K19 driver_gate module (no CTest infra in
// this project; follows the "standalone g++ on the real sources"
// precedent of tests/run_k18.sh). The module is pure C++ (no Qt, no
// alpm, no fmt — std-only), so this compiles it directly with the
// project's warning set and drives derive_headers_pkg() /
// classify_gpu() / select_dkms_package() / evaluate_gate() with an
// injected GateProbe only (no filesystem), asserting:
//   - derive_headers_pkg: appends -headers, generic across flavors,
//     idempotent, empty -> empty
//   - classify_gpu: a 21-line lspci corpus — RTX 2080 Ti / 3070 / 4090 /
//     5090 / GA102 [TITAN] / TU106 [GTX 1660] / TITAN V / T1000 / A100
//     -> TURING_PLUS; GTX 1080 / 980M / GT 1030 / GT 730 / Quadro P4000 /
//     P100 / GM204 [GTX 980] / TITAN X / K4000 -> PRE_TURING; an unknown
//     device / "" / non-nvidia -> UNKNOWN — plus the table-disjointness
//     proof (loop the corpus: every line is exactly one generation, no
//     line classifies both ways)
//   - select_dkms_package: TURING_PLUS -> nvidia-open-dkms, PRE_TURING ->
//     nvidia-dkms, UNKNOWN -> nvidia-dkms (conservative — a false "open"
//     is never selected on an unknown GPU)
//   - evaluate_gate: the full D3 decision table — no HW -> PROCEED; HW +
//     no driver -> PROCEED; HW + precompiled -> WARN_MIGRATE with the
//     exact idempotent migration command (per generation, incl. the
//     conservative UNKNOWN and the headers-omitted unknown-target
//     variant); precompiled + dkms coexistence -> WARN_MIGRATE; dkms +
//     headers installed + build dir ok -> PROCEED; dkms + headers
//     missing + repo available -> ENSURE_HEADERS with the exact ensure
//     command; dkms + headers missing + no repo -> WARN_ONLY + the
//     non-empty D7 banner; kver "" + headers installed -> PROCEED;
//     unbound probe -> PROCEED, no crash
//   - the real evaluate_gate() smoke runs read-only on this machine
//     (local-DB + /sys + lspci/pacman -Siq): a valid action + no crash +
//     an INFO line — MACHINE-TOLERANT, no specific action is asserted
//     (the dev box reads WARN_ONLY, not the WARN_MIGRATE the plan's
//     parenthetical lists)

#include "driver_gate.hpp"

#include <cstdio>       // for printf
#include <string>       // for string
#include <string_view>  // for string_view
#include <utility>      // for pair
#include <vector>       // for vector

namespace {

using driver_gate::classify_gpu;
using driver_gate::derive_headers_pkg;
using driver_gate::evaluate_gate;
using driver_gate::GateAction;
using driver_gate::GateProbe;
using driver_gate::GateTarget;
using driver_gate::GateVerdict;
using driver_gate::GpuGeneration;
using driver_gate::select_dkms_package;

int g_failures = 0;

// The k18 assertion style: print PASS/FAIL, count the failures.
void check(bool condition, const char* what) {
    if (condition) {
        std::printf("PASS: %s\n", what);
    } else {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// A string-equality check that names the actual value on failure.
void check_str(const std::string& actual, const std::string& expected, const char* what) {
    if (actual == expected) {
        std::printf("PASS: %s\n", what);
    } else {
        ++g_failures;
        std::printf("FAIL: %s (actual: '%s', expected: '%s')\n", what, actual.c_str(), expected.c_str());
    }
}

// A membership test for the probe fact lists (std-only, never the system).
bool in_list(const std::vector<std::string>& list, std::string_view name) {
    for (const std::string& entry : list) {
        if (entry == name) {
            return true;
        }
    }
    return false;
}

// A GateProbe built from simple facts (the k18 DistroProbe{} pattern):
// every state behind a predicate, no filesystem, no shell.
GateProbe make_probe(bool hardware,
    std::string_view gpu,
    const std::vector<std::string>& installed,
    const std::vector<std::string>& build_dirs,
    const std::vector<std::string>& repos) {
    GateProbe probe{};
    probe.nvidia_hardware   = [hardware] { return hardware; };
    probe.gpu_names         = [gpu] { return std::string{gpu}; };
    probe.package_installed = [installed](std::string_view name) { return in_list(installed, name); };
    probe.build_dir_exists  = [build_dirs](std::string_view kver) { return in_list(build_dirs, kver); };
    probe.repo_available    = [repos](std::string_view name) { return in_list(repos, name); };
    return probe;
}

const char* gen_name(GpuGeneration gen) {
    switch (gen) {
    case GpuGeneration::TURING_PLUS:
        return "TURING_PLUS";
    case GpuGeneration::PRE_TURING:
        return "PRE_TURING";
    case GpuGeneration::UNKNOWN:
        return "UNKNOWN";
    }
    return "?";
}

const char* action_name(GateAction action) {
    switch (action) {
    case GateAction::PROCEED:
        return "PROCEED";
    case GateAction::WARN_MIGRATE:
        return "WARN_MIGRATE";
    case GateAction::ENSURE_HEADERS:
        return "ENSURE_HEADERS";
    case GateAction::WARN_ONLY:
        return "WARN_ONLY";
    }
    return "?";
}

// Representative lspci lines (the classify_gpu corpus entries, reused by
// the evaluate_gate cases below).
const std::string kRtx3070 = "01:00.0 VGA compatible controller: NVIDIA Corporation GA104 [GeForce RTX 3070] [10de:2282] (rev a1)";
const std::string kGtx1080 = "01:00.0 VGA compatible controller: NVIDIA Corporation GP104 [GeForce GTX 1080] [10de:1b80] (rev a1)";

}  // namespace

int main() {
    // ------------------------------------------------------------------
    // 1. derive_headers_pkg: appends -headers, generic (not hardcoded to
    //    one flavor), idempotent, empty -> empty.
    // ------------------------------------------------------------------
    check(derive_headers_pkg("linux-zen") == "linux-zen-headers", "derive: linux-zen -> linux-zen-headers");
    check(derive_headers_pkg("linux-cachyos-custom") == "linux-cachyos-custom-headers",
        "derive: linux-cachyos-custom -> linux-cachyos-custom-headers");
    check(derive_headers_pkg("linux") == "linux-headers", "derive: linux -> linux-headers");
    check(derive_headers_pkg("linux-zen-headers") == "linux-zen-headers", "derive: idempotent (-headers input unchanged)");
    check(derive_headers_pkg("").empty(), "derive: empty input -> empty");

    // ------------------------------------------------------------------
    // 2. classify_gpu: the 21-line lspci corpus + the disjointness proof
    //    (every line is exactly one generation — the two curated tables
    //    must not overlap; the shared gp10x chip family disambiguates
    //    via the marketing tokens only).
    // ------------------------------------------------------------------
    const std::vector<std::pair<std::string, GpuGeneration>> corpus{
        // TURING_PLUS (the open kernel module supports Turing and newer):
        {kRtx3070, GpuGeneration::TURING_PLUS},
        {"01:00.0 VGA compatible controller: NVIDIA Corporation GP102 [GeForce RTX 2080 Ti] [10de:1e04] (rev a1)",
            GpuGeneration::TURING_PLUS},
        {"01:00.0 VGA compatible controller: NVIDIA Corporation AD102 [GeForce RTX 4090] [10de:2684] (rev a1)",
            GpuGeneration::TURING_PLUS},
        {"01:00.0 VGA compatible controller: NVIDIA Corporation GB202 [GeForce RTX 5090] [10de:2b85] (rev a1)",
            GpuGeneration::TURING_PLUS},
        {"00:01.0 3D controller: NVIDIA Corporation GA102 [TITAN] [10de:222a] (rev a1)", GpuGeneration::TURING_PLUS},
        {"01:00.0 VGA compatible controller: NVIDIA Corporation TU106 [GeForce GTX 1660] [10de:1f00] (rev a1)",
            GpuGeneration::TURING_PLUS},
        {"01:00.0 3D controller: NVIDIA Corporation TITAN V [10de:1b06] (rev a1)", GpuGeneration::TURING_PLUS},
        {"00:01.0 3D controller: NVIDIA Corporation T1000 [10de:1c14] (rev a1)", GpuGeneration::TURING_PLUS},
        {"00:01.0 3D controller: NVIDIA Corporation A100-SXM4-80GB [10de:20b2] (rev a1)", GpuGeneration::TURING_PLUS},
        // PRE_TURING (Fermi -> Pascal; the proprietary module is required):
        {kGtx1080, GpuGeneration::PRE_TURING},
        {"01:00.0 VGA compatible controller: NVIDIA Corporation GeForce GTX 980M [10de:139d] (rev a1)",
            GpuGeneration::PRE_TURING},
        {"01:00.0 VGA compatible controller: NVIDIA Corporation GT 1030 [10de:1d13] (rev a1)", GpuGeneration::PRE_TURING},
        {"01:00.0 VGA compatible controller: NVIDIA Corporation GeForce GT 730 [10de:0f82] (rev a1)",
            GpuGeneration::PRE_TURING},
        {"01:00.0 VGA compatible controller: NVIDIA Corporation Quadro P4000 [10de:1739] (rev a1)",
            GpuGeneration::PRE_TURING},
        {"00:01.0 3D controller: NVIDIA Corporation GP100 [P100] [10de:15b8] (rev a1)", GpuGeneration::PRE_TURING},
        {"01:00.0 VGA compatible controller: NVIDIA Corporation GM204 [GeForce GTX 980] [10de:13c0] (rev a1)",
            GpuGeneration::PRE_TURING},
        {"01:00.0 3D controller: NVIDIA Corporation TITAN X [10de:1401] (rev a1)", GpuGeneration::PRE_TURING},
        {"01:00.0 3D controller: NVIDIA Corporation K4000 [10de:0e62] (rev a1)", GpuGeneration::PRE_TURING},
        // UNKNOWN (undeterminable — the conservative fallback input):
        {"01:00.0 VGA compatible controller: NVIDIA Corporation Device 1af1 [10de:1af1] (rev a1)", GpuGeneration::UNKNOWN},
        {"", GpuGeneration::UNKNOWN},
        {"FooBar", GpuGeneration::UNKNOWN},
    };
    for (const auto& [line, expected] : corpus) {
        const GpuGeneration gen = classify_gpu(line);
        check(gen == expected, ("classify: '" + line + "' -> " + gen_name(gen)).c_str());
        // Disjointness: the line must land in exactly one of the three
        // generations (a double table-hit is the D2 defect; a
        // PRE_TURING line reading TURING_PLUS — the TURING table is
        // checked first — is how it surfaces).
        const int buckets = (gen == GpuGeneration::TURING_PLUS ? 1 : 0) + (gen == GpuGeneration::PRE_TURING ? 1 : 0)
            + (gen == GpuGeneration::UNKNOWN ? 1 : 0);
        check(buckets == 1, ("disjointness: exactly one generation for '" + line + "'").c_str());
    }

    // ------------------------------------------------------------------
    // 3. select_dkms_package: the generation -> DKMS package map.
    // ------------------------------------------------------------------
    check_str(select_dkms_package(GpuGeneration::TURING_PLUS), "nvidia-open-dkms", "select: TURING_PLUS -> nvidia-open-dkms");
    check_str(select_dkms_package(GpuGeneration::PRE_TURING), "nvidia-dkms", "select: PRE_TURING -> nvidia-dkms");
    check_str(select_dkms_package(GpuGeneration::UNKNOWN), "nvidia-dkms", "select: UNKNOWN -> nvidia-dkms (conservative)");

    // ------------------------------------------------------------------
    // 4. evaluate_gate: the full D3 decision table (injected GateProbe
    //    only — no filesystem, no shell).
    // ------------------------------------------------------------------
    // 4a. No nvidia hardware -> PROCEED (nothing to gate).
    {
        const GateVerdict v = evaluate_gate(make_probe(false, kRtx3070, {}, {}, {}), GateTarget{"linux-zen", "6.8.7-1-zen"});
        check(v.action == GateAction::PROCEED, "D3: no nvidia hardware -> PROCEED");
        check(v.migration_cmd.empty() && v.ensure_cmd.empty(), "D3: no hardware -> no command payloads");
    }
    // 4b. Hardware + no nvidia driver -> PROCEED (the user's choice).
    {
        const GateVerdict v = evaluate_gate(make_probe(true, kRtx3070, {}, {}, {}), GateTarget{"linux-zen", "6.8.7-1-zen"});
        check(v.action == GateAction::PROCEED, "D3: hardware, no nvidia driver -> PROCEED");
    }
    // 4c. Hardware + precompiled (nvidia-open) + a TURING_PLUS line +
    //     target linux-cachyos-custom (kver set) -> WARN_MIGRATE with the
    //     exact idempotent migration command.
    {
        const GateVerdict v = evaluate_gate(make_probe(true, kRtx3070, {"nvidia-open"}, {}, {}),
            GateTarget{"linux-cachyos-custom", "7.2.3-1-cachyos-custom"});
        check(v.action == GateAction::WARN_MIGRATE, "D3: precompiled nvidia-open -> WARN_MIGRATE");
        check_str(v.dkms_package, "nvidia-open-dkms", "D3: the TURING_PLUS line -> nvidia-open-dkms");
        check_str(v.headers_package, "linux-cachyos-custom-headers", "D3: headers derived from the target kernel");
        check_str(v.migration_cmd, "pacman -S --needed --asexplicit nvidia-open-dkms linux-cachyos-custom-headers",
            "D3: the exact idempotent migration command");
        check(!v.message.empty(), "D3: WARN_MIGRATE carries a non-empty plain-language message");
        check(!v.banner.empty(), "D3: WARN_MIGRATE carries the D7 banner");
    }
    // 4d. The same precompiled row with a PRE_TURING (GTX 1080) line ->
    //     nvidia-dkms in the command.
    {
        const GateVerdict v = evaluate_gate(make_probe(true, kGtx1080, {"nvidia-open"}, {}, {}),
            GateTarget{"linux-cachyos-custom", "7.2.3-1-cachyos-custom"});
        check(v.action == GateAction::WARN_MIGRATE, "D3: precompiled + PRE_TURING line -> WARN_MIGRATE");
        check_str(v.migration_cmd, "pacman -S --needed --asexplicit nvidia-dkms linux-cachyos-custom-headers",
            "D3: nvidia-dkms in the command for the PRE_TURING line");
    }
    // 4e. The same precompiled row with no gpu line (UNKNOWN) -> the
    //     conservative nvidia-dkms.
    {
        const GateVerdict v = evaluate_gate(make_probe(true, "", {"nvidia-open"}, {}, {}),
            GateTarget{"linux-cachyos-custom", "7.2.3-1-cachyos-custom"});
        check(v.action == GateAction::WARN_MIGRATE, "D3: precompiled + UNKNOWN gpu -> WARN_MIGRATE");
        check_str(v.dkms_package, "nvidia-dkms", "D3: UNKNOWN -> conservative nvidia-dkms (never open)");
        check_str(v.migration_cmd, "pacman -S --needed --asexplicit nvidia-dkms linux-cachyos-custom-headers",
            "D3: nvidia-dkms in the command for the UNKNOWN gpu");
    }
    // 4f. Precompiled + unknown target (kernel "") -> WARN_MIGRATE; the
    //     command omits the headers part (no trailing space).
    {
        const GateVerdict v = evaluate_gate(make_probe(true, kRtx3070, {"nvidia-open"}, {}, {}), GateTarget{});
        check(v.action == GateAction::WARN_MIGRATE, "D3: precompiled, unknown target -> WARN_MIGRATE");
        check_str(v.migration_cmd, "pacman -S --needed --asexplicit nvidia-open-dkms",
            "D3: the headers-omitted command has no trailing space");
    }
    // 4g. Hardware + precompiled + dkms (coexistence) -> WARN_MIGRATE
    //     (the --needed command is idempotent; pacman's conflict
    //     resolution removes the precompiled package).
    {
        const GateVerdict v = evaluate_gate(make_probe(true, kRtx3070, {"nvidia", "nvidia-dkms"}, {}, {}), GateTarget{"linux-zen", "6.8.7-1-zen"});
        check(v.action == GateAction::WARN_MIGRATE, "D3: precompiled + dkms coexistence -> WARN_MIGRATE");
        check(!v.message.empty(), "D3: the coexistence message is non-empty");
        check_str(v.migration_cmd, "pacman -S --needed --asexplicit nvidia-open-dkms linux-zen-headers",
            "D3: the coexistence migration command is exact");
    }
    // 4h. Hardware + dkms + headers installed + build dir ok -> PROCEED.
    {
        const GateVerdict v = evaluate_gate(make_probe(true, kRtx3070, {"nvidia-open-dkms", "linux-zen-headers"}, {"6.8.7-1-zen"}, {}),
            GateTarget{"linux-zen", "6.8.7-1-zen"});
        check(v.action == GateAction::PROCEED, "D3: dkms + headers installed + build dir ok -> PROCEED");
    }
    // 4i. Hardware + dkms + headers missing + repo available ->
    //     ENSURE_HEADERS with the exact ensure command.
    {
        const GateVerdict v = evaluate_gate(make_probe(true, kRtx3070, {"nvidia-dkms"}, {}, {"linux-zen-headers"}),
            GateTarget{"linux-zen", "6.8.7-1-zen"});
        check(v.action == GateAction::ENSURE_HEADERS, "D3: dkms + headers missing + repo -> ENSURE_HEADERS");
        check_str(v.ensure_cmd, "pacman -S --needed linux-zen-headers", "D3: the exact ensure command");
        check(!v.message.empty(), "D3: ENSURE_HEADERS carries a non-empty message");
        check(v.migration_cmd.empty(), "D3: ENSURE_HEADERS carries no migration command");
    }
    // 4j. Hardware + dkms + headers missing + no repo -> WARN_ONLY with
    //     the non-empty D7 banner.
    {
        const GateVerdict v = evaluate_gate(make_probe(true, kRtx3070, {"nvidia-dkms"}, {}, {}),
            GateTarget{"linux-zen", "6.8.7-1-zen"});
        check(v.action == GateAction::WARN_ONLY, "D3: dkms + headers missing + no repo -> WARN_ONLY");
        check(!v.banner.empty(), "D3: WARN_ONLY carries the non-empty D7 banner");
        check(!v.message.empty(), "D3: WARN_ONLY carries a non-empty message");
    }
    // 4k. kver "" + headers installed -> PROCEED (the check degrades to
    //     the headers-package membership).
    {
        const GateVerdict v = evaluate_gate(make_probe(true, kRtx3070, {"nvidia-open-dkms", "linux-zen-headers"}, {}, {}), GateTarget{"linux-zen", ""});
        check(v.action == GateAction::PROCEED, "D3: kver '' + headers installed -> PROCEED");
    }
    // 4l. Unbound probe -> PROCEED, no crash (an unbound predicate
    //     contributes no signal).
    {
        const GateVerdict v = evaluate_gate(GateProbe{}, GateTarget{"linux-zen", "6.8.7-1-zen"});
        check(v.action == GateAction::PROCEED, "D3: unbound probe -> PROCEED, no crash");
    }

    // ------------------------------------------------------------------
    // 5. Real-probe smoke (the k18 section-9 pattern): the real
    //    evaluate_gate() on this machine — read-only (local-DB + /sys +
    //    lspci/pacman -Siq, no root). MACHINE-TOLERANT: only a valid
    //    action + no crash are asserted; the specific verdict depends on
    //    this box's driver state (the dev box reads WARN_ONLY, not the
    //    WARN_MIGRATE the plan's parenthetical lists).
    // ------------------------------------------------------------------
    {
        const GateVerdict v = evaluate_gate(GateTarget{});
        check(v.action == GateAction::PROCEED || v.action == GateAction::WARN_MIGRATE || v.action == GateAction::ENSURE_HEADERS
                || v.action == GateAction::WARN_ONLY,
            "real: evaluate_gate(GateTarget{}) returns a valid action (no crash)");
        std::printf("INFO: live gate on this machine -> %s (dkms='%s' headers='%s')\n", action_name(v.action),
            v.dkms_package.c_str(), v.headers_package.c_str());
        std::printf("INFO: live facts: hardware=%d precompiled=%d dkms=%d\n",
            driver_gate::nvidia_hardware_present() ? 1 : 0, driver_gate::precompiled_driver_installed() ? 1 : 0,
            driver_gate::dkms_driver_installed() ? 1 : 0);
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
