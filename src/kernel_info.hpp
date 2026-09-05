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

#ifndef KERNEL_INFO_HPP
#define KERNEL_INFO_HPP

#include <functional>   // for function
#include <string>       // for string
#include <string_view>  // for string_view
#include <vector>       // for vector

// The Qt-free booted-kernel info extractor: the 15 display-ready facts the
// MainWindow "Active Kernel Information" header renders — kernel & toolchain,
// CPU arch target, optimization & LTO, scheduling & latency, and runtime
// subsystems. The detection facts come from /proc, /sys, /boot, and the
// modinfo/llvm-objdump tooling — read-only, and all of it behind injectable
// predicates (the GateProbe precedent in driver_gate.hpp), so the pure
// extraction is testable without a filesystem. An unbound predicate
// contributes no signal ("" / empty vector), never a crash.
namespace kernel_info {

// Injection point (the DistroProbe/GateProbe precedent): every system fact
// behind a predicate. A predicate may be left unbound (default-constructed)
// and the corresponding field degrades to "" (or an empty module list)
// instead of throwing — the module never reads a file or shell directly,
// only through these.
struct KernelInfoProbe {
    std::function<std::string()> uname_r;                          // the booted release (/proc/sys/kernel/osrelease)
    std::function<std::string()> uname_v;                          // the build-date portion of /proc/version (the "#" on)
    std::function<std::string()> proc_version;                     // the whole /proc/version line
    std::function<std::string()> kconfig;                          // the booted kernel's config text (zcat /proc/config.gz or /boot/config-<release>)
    std::function<std::string(std::string_view)> sys_read;         // read one /sys (or /proc/sys) attribute file by path
    std::function<std::string(std::string_view)> sysctl_get;       // read one sysctl value by dotted key
    std::function<std::vector<std::string>()> loaded_modules;      // the loaded module names, in lsmod order
    std::function<std::string(std::string_view)> module_filename;  // a module's on-disk filename (modinfo -F filename)
    std::function<std::string(std::string_view)> disassemble;      // a module file's disassembly text (llvm-objdump -d)
};

// The 15 display-ready booted-kernel facts (empty = unknown / not available;
// the UI renders an empty value as gray). The strings are display-ready —
// the values the header shows verbatim, no further formatting in the Qt
// layer.
struct KernelInfo {
    std::string release;                 // "7.2.3-1-cachyos-custom"
    std::string build_date;              // "#1 SMP PREEMPT_DYNAMIC Thu, 03 Sep 2026 21:56:27 +0000"
    std::string compiler;                // "clang 22.1.8" / "gcc 13.2.1"
    std::string target_arch;             // "Native" / "Family Optimized [CONFIG_M...]" / "Generic" / "Custom"
    std::string isa_level;               // "x86-64-v<N>" ("x86-64-v1" = the baseline; "" = not configured)
    std::string instruction_validation;  // "BMI2/AVX2 verified" ("not checked" when no module disassembled)
    std::string lto_status;              // "Full LTO" / "Thin LTO" / "None"
    std::string optimization_flag;       // "-O3" / "-O2"
    std::string tick_rate;               // "1000 Hz"
    std::string sched_ext;               // "Enabled" / "Disabled"
    std::string preemption_model;        // the sysfs word, else "Dynamic" / "Full" / "Voluntary"
    std::string mglru;                   // "Active (<val>)" / "Disabled"
    std::string thp;                     // "always" / "madvise" / "never" (the bracketed value)
    std::string tcp_congestion;          // e.g. "bbr"
    std::string clocksource;             // e.g. "tsc"
};

// The pure extraction (probe-injected, [[nodiscard]]): every field is
// derived independently from the injected strings; an unbound predicate
// degrades that field to "" (no signal, no crash). No rule reads a
// filesystem, calls a shell, or throws.
[[nodiscard]] KernelInfo extract_kernel_info(const KernelInfoProbe& probe);

// The real overload (wired to disk — the driver_gate "real overload"
// precedent): binds every KernelInfoProbe predicate to its std-only system
// read and runs the pure extraction once.
[[nodiscard]] KernelInfo extract_kernel_info();

}  // namespace kernel_info

#endif  // KERNEL_INFO_HPP
