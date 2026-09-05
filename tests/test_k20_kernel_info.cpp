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

// Standalone unit test for the K20 kernel_info module (no CTest infra in
// this project; follows the "standalone g++ on the real sources"
// precedent of tests/run_k18.sh). The module is pure C++ (no Qt, no
// alpm, no fmt — std-only), so this compiles it directly with the
// project's warning set and drives extract_kernel_info() with an
// injected KernelInfoProbe only (no filesystem), asserting:
//   - empty probe: all 15 fields empty (the no-signal, no-crash
//     degradation rule — an unbound predicate is inert, and an empty
//     kconfig makes no false "None"/"Disabled"/"Custom" claims)
//   - kernel & toolchain: release verbatim, build_date from the
//     uname_v string, the compiler from the /proc/version line (the
//     word-boundary + version-token rules, "gcc 14.2.1" from a
//     "(gcc 14.2.1 ...)" line)
//   - CPU arch target: X86_NATIVE_CPU -> "Native"; a curated family
//     line (CONFIG_MZEN=y) -> "Family Optimized [CONFIG_MZEN]";
//     GENERIC_CPU -> "Generic"; ISA level via the 5.17+ boolean form
//     (CONFIG_X86_64_V3=y -> "x86-64-v3" — the int form
//     CONFIG_X86_64_VERSION=3 is not a =y line and is invisible to
//     the =y-only scanner, the boolean symbol is the display form)
//   - optimization & LTO: LTO_CLANG_FULL -> "Full LTO";
//     LTO_CLANG_THIN -> "Thin LTO"; neither -> "None"; _O3 -> "-O3";
//     _PERFORMANCE without _O3 -> "-O2" (the exact-key line-start
//     tests never cross-match); CONFIG_HZ=1000 -> "1000 Hz";
//     SCHED_CLASS_EXT -> "Enabled" / absent (non-empty config) ->
//     "Disabled"
//   - scheduling: no sysfs preempt word (the root-only file, an
//     unbound probe) -> the kconfig fallback: PREEMPT_DYNAMIC ->
//     "Dynamic"; PREEMPT without _DYNAMIC -> "Full" (the exact-key
//     test never cross-matches)
//   - runtime subsystems: lru_gen "0x0001" -> "Active (0x0001)";
//     "0x0000" -> "Disabled"; THP "always madvise [never]" ->
//     "never" (the bracketed word); clocksource "tsc" -> "tsc";
//     sysctl tcp_congestion_control "bbr" -> "bbr"
//   - the real extract_kernel_info() smoke runs read-only on this
//     machine (zcat /proc/config.gz + sysctl + lsmod + modinfo +
//     llvm-objdump, no root): no crash + a non-empty release +
//     lto_status in the valid set — MACHINE-TOLERANT, no specific
//     value is asserted (the dev box reads its own live kernel)

#include "kernel_info.hpp"

#include <cstdio>       // for printf
#include <string>       // for string
#include <string_view>  // for string_view

namespace {

using kernel_info::extract_kernel_info;
using kernel_info::KernelInfo;
using kernel_info::KernelInfoProbe;

int g_failures = 0;
int g_checks   = 0;

// The k18/k19 assertion style: print PASS/FAIL, count the checks and
// the failures.
void check(bool condition, const char* what) {
    ++g_checks;
    if (condition) {
        std::printf("PASS: %s\n", what);
    } else {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// A KernelInfoProbe built from simple strings (the k19 GateProbe{}
// pattern): every fact behind a predicate, no filesystem, no shell. A
// predicate stays unbound when its value is empty, and the module's
// degradation rule (no signal, no crash) keeps it inert.
KernelInfoProbe make_probe(std::string uname_r = "",
    std::string uname_v                        = "",
    std::string proc_version                   = "",
    std::string kconfig                        = "",
    std::string mglru_val                      = "",
    std::string thp_val                        = "",
    std::string clocksource                    = "",
    std::string tcp_cc                         = "") {
    KernelInfoProbe p{};
    if (!uname_r.empty()) {
        p.uname_r = [uname_r] { return uname_r; };
    }
    if (!uname_v.empty()) {
        p.uname_v = [uname_v] { return uname_v; };
    }
    if (!proc_version.empty()) {
        p.proc_version = [proc_version] { return proc_version; };
    }
    if (!kconfig.empty()) {
        p.kconfig = [kconfig] { return kconfig; };
    }
    if (!mglru_val.empty() || !thp_val.empty() || !clocksource.empty()) {
        p.sys_read = [mglru_val, thp_val, clocksource](std::string_view path) -> std::string {
            if (path.find("lru_gen") != std::string_view::npos) {
                return mglru_val;
            }
            if (path.find("transparent_hugepage") != std::string_view::npos) {
                return thp_val;
            }
            if (path.find("clocksource") != std::string_view::npos) {
                return clocksource;
            }
            return "";
        };
    }
    if (!tcp_cc.empty()) {
        p.sysctl_get = [tcp_cc](std::string_view) -> std::string { return tcp_cc; };
    }
    return p;
}

}  // namespace

int main() {
    // ------------------------------------------------------------------
    // 1a. Empty probe: all 15 fields empty — the no-signal, no-crash
    //     degradation rule (an unbound predicate is inert; an empty
    //     kconfig makes no false "None"/"Disabled"/"Custom" claims).
    // ------------------------------------------------------------------
    {
        const KernelInfo info = extract_kernel_info(KernelInfoProbe{});
        check(info.release.empty() && info.build_date.empty() && info.compiler.empty()
                && info.target_arch.empty() && info.isa_level.empty() && info.instruction_validation.empty()
                && info.lto_status.empty() && info.optimization_flag.empty() && info.tick_rate.empty()
                && info.sched_ext.empty() && info.preemption_model.empty() && info.mglru.empty()
                && info.thp.empty() && info.tcp_congestion.empty() && info.clocksource.empty(),
            "1a: empty probe -> all 15 fields empty (no signal, no crash)");
    }

    // ------------------------------------------------------------------
    // 1b. Kernel & toolchain: release verbatim, build_date from
    //     uname_v, the compiler from the /proc/version line.
    // ------------------------------------------------------------------
    {
        const KernelInfo info = extract_kernel_info(make_probe("6.8.7-1-zen", "#1 SMP PREEMPT_DYNAMIC", "Linux version 6.8.7-1-zen (madgoat@devbox) (gcc 14.2.1 20240914)"));
        check(info.release == "6.8.7-1-zen", "1b: release verbatim from uname_r");
        check(!info.build_date.empty(), "1b: build_date non-empty from uname_v");
        check(info.compiler == "gcc 14.2.1", "1b: compiler 'gcc 14.2.1' from the /proc/version line");
    }

    // ------------------------------------------------------------------
    // 1c-1f. CPU arch target + ISA level (kconfig-derived).
    // ------------------------------------------------------------------
    {
        const KernelInfo info = extract_kernel_info(make_probe("", "", "", "CONFIG_X86_NATIVE_CPU=y"));
        check(info.target_arch == "Native", "1c: X86_NATIVE_CPU -> 'Native'");
    }
    {
        const KernelInfo info = extract_kernel_info(make_probe("", "", "", "CONFIG_MZEN=y"));
        check(info.target_arch == "Family Optimized [CONFIG_MZEN]", "1d: a curated family line (MZEN) -> 'Family Optimized [CONFIG_MZEN]'");
    }
    {
        const KernelInfo info = extract_kernel_info(make_probe("", "", "", "CONFIG_GENERIC_CPU=y"));
        check(info.target_arch == "Generic", "1e: GENERIC_CPU -> 'Generic'");
    }
    // The 5.17+ boolean form: the int CONFIG_X86_64_VERSION=3 is not a
    // =y line and is invisible to the =y-only scanner, so the boolean
    // symbol is the form that produces the display string.
    {
        const KernelInfo info = extract_kernel_info(make_probe("", "", "", "CONFIG_X86_64_V3=y"));
        check(info.isa_level == "x86-64-v3", "1f: X86_64_V3 -> 'x86-64-v3'");
        check(info.isa_level.find("v3") != std::string::npos, "1f: isa_level contains 'v3'");
    }

    // ------------------------------------------------------------------
    // 1g-1i. LTO status (kconfig-derived): full / thin / none.
    // ------------------------------------------------------------------
    {
        const KernelInfo info = extract_kernel_info(make_probe("", "", "", "CONFIG_LTO_CLANG_FULL=y"));
        check(info.lto_status == "Full LTO", "1g: LTO_CLANG_FULL -> 'Full LTO'");
    }
    {
        const KernelInfo info = extract_kernel_info(make_probe("", "", "", "CONFIG_LTO_CLANG_THIN=y"));
        check(info.lto_status == "Thin LTO", "1h: LTO_CLANG_THIN -> 'Thin LTO'");
    }
    {
        const KernelInfo info = extract_kernel_info(make_probe("", "", "", "CONFIG_CC_OPTIMIZE_FOR_PERFORMANCE=y"));
        check(info.lto_status == "None", "1i: neither LTO symbol -> 'None'");
    }

    // ------------------------------------------------------------------
    // 1j-1k. Optimization flag: the exact-key line-start tests never
    //     cross-match (_O3 is checked first, and "CONFIG_..._O3=" is
    //     not a substring of the plain "_PERFORMANCE=" line).
    // ------------------------------------------------------------------
    {
        const KernelInfo info = extract_kernel_info(make_probe("", "", "", "CONFIG_CC_OPTIMIZE_FOR_PERFORMANCE_O3=y"));
        check(info.optimization_flag == "-O3", "1j: _PERFORMANCE_O3 -> '-O3'");
    }
    {
        const KernelInfo info = extract_kernel_info(make_probe("", "", "", "CONFIG_CC_OPTIMIZE_FOR_PERFORMANCE=y"));
        check(info.optimization_flag == "-O2", "1k: _PERFORMANCE without _O3 -> '-O2'");
    }

    // ------------------------------------------------------------------
    // 1l-1n. Tick rate + sched_ext (kconfig-derived).
    // ------------------------------------------------------------------
    {
        const KernelInfo info = extract_kernel_info(make_probe("", "", "", "CONFIG_HZ=1000"));
        check(info.tick_rate == "1000 Hz", "1l: CONFIG_HZ=1000 -> '1000 Hz'");
    }
    {
        const KernelInfo info = extract_kernel_info(make_probe("", "", "", "CONFIG_SCHED_CLASS_EXT=y"));
        check(info.sched_ext == "Enabled", "1m: SCHED_CLASS_EXT -> 'Enabled'");
    }
    {
        const KernelInfo info = extract_kernel_info(make_probe("", "", "", "CONFIG_HZ=100"));
        check(info.sched_ext == "Disabled", "1n: no SCHED_CLASS_EXT (non-empty config) -> 'Disabled'");
    }

    // ------------------------------------------------------------------
    // 1o-1p. Preemption model: the root-only sysfs word is absent (an
    //     unbound probe), so the kconfig fallback decides —
    //     PREEMPT_DYNAMIC first, then PREEMPT (the exact-key test
    //     never cross-matches "CONFIG_PREEMPT_DYNAMIC=").
    // ------------------------------------------------------------------
    {
        const KernelInfo info = extract_kernel_info(make_probe("", "", "", "CONFIG_PREEMPT_DYNAMIC=y"));
        check(info.preemption_model == "Dynamic", "1o: PREEMPT_DYNAMIC -> 'Dynamic' (the sysfs word absent)");
    }
    {
        const KernelInfo info = extract_kernel_info(make_probe("", "", "", "CONFIG_PREEMPT=y"));
        check(info.preemption_model == "Full", "1p: PREEMPT without _DYNAMIC -> 'Full'");
    }

    // ------------------------------------------------------------------
    // 1q-1u. Runtime subsystems (sys + sysctl).
    // ------------------------------------------------------------------
    {
        const KernelInfo info = extract_kernel_info(make_probe("", "", "", "", "0x0001"));
        check(info.mglru == "Active (0x0001)", "1q: lru_gen '0x0001' -> 'Active (0x0001)'");
    }
    {
        const KernelInfo info = extract_kernel_info(make_probe("", "", "", "", "0x0000"));
        check(info.mglru == "Disabled", "1r: lru_gen '0x0000' -> 'Disabled'");
    }
    {
        const KernelInfo info = extract_kernel_info(make_probe("", "", "", "", "", "always madvise [never]"));
        check(info.thp == "never", "1s: THP 'always madvise [never]' -> 'never' (the bracketed word)");
    }
    {
        const KernelInfo info = extract_kernel_info(make_probe("", "", "", "", "", "", "tsc"));
        check(info.clocksource == "tsc", "1t: clocksource 'tsc' -> 'tsc'");
    }
    {
        const KernelInfo info = extract_kernel_info(make_probe("", "", "", "", "", "", "", "bbr"));
        check(info.tcp_congestion == "bbr", "1u: sysctl tcp_congestion_control 'bbr' -> 'bbr'");
    }

    // ------------------------------------------------------------------
    // 2. Real-probe smoke (the k19 section-5 pattern): the real
    //    extract_kernel_info() on this machine — read-only (zcat /proc/
    //    config.gz + sysctl + lsmod + modinfo + llvm-objdump, no root).
    //    MACHINE-TOLERANT: only no crash + a non-empty release +
    //    lto_status in the valid set are asserted ("" = no config, or
    //    the three LTO display strings); the specific values depend on
    //    this box's live kernel.
    // ------------------------------------------------------------------
    {
        const KernelInfo info = extract_kernel_info();
        check(!info.release.empty(), "real: extract_kernel_info() returns a non-empty release (no crash)");
        check(info.lto_status == "Full LTO" || info.lto_status == "Thin LTO" || info.lto_status == "None" || info.lto_status.empty(),
            "real: lto_status is in the valid set {Full LTO, Thin LTO, None, ''}");
        std::printf("INFO: live kernel info on this machine: release='%s' compiler='%s' arch='%s' isa='%s' lto='%s' opt='%s' tick='%s' "
                    "sched_ext='%s' preempt='%s' mglru='%s' thp='%s' tcp='%s' clock='%s'\n",
            info.release.c_str(), info.compiler.c_str(), info.target_arch.c_str(), info.isa_level.c_str(), info.lto_status.c_str(),
            info.optimization_flag.c_str(), info.tick_rate.c_str(), info.sched_ext.c_str(), info.preemption_model.c_str(), info.mglru.c_str(),
            info.thp.c_str(), info.tcp_congestion.c_str(), info.clocksource.c_str());
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED (%d checks)\n", g_checks);
        return 0;
    }
    std::printf("FAILED: %d/%d\n", g_failures, g_checks);
    return 1;
}
