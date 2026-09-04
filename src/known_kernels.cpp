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

#include "known_kernels.hpp"

#include <algorithm>    // for unique
#include <ranges>       // for ranges::find_if, sort
#include <string_view>  // for string_view

namespace km {

namespace {

    // Humanize a kernel name for synthesized fallback text
    // ("linux-rc" -> "Linux Rc", "linux" -> "Linux").
    auto humanize_name(std::string_view name) -> std::string {
        std::string label{};
        label.reserve(name.size() + 1);
        bool capitalize_next{true};
        for (const char c : name) {
            if (c == '-') {
                label += ' ';
                capitalize_next = true;
            } else {
                label += (capitalize_next && c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
                capitalize_next = false;
            }
        }
        return label;
    }

}  // namespace

const std::vector<KnownKernel>& known_kernels() {
    // The curated kernel table. Every kernel maps to its own package as its
    // build source (identity mapping) — there is no cross-package override.
    // Fields per row: name, display_name, description, default_source
    // (== name), install_package, install_repo, precompiled_available,
    // buildable.
    static const std::vector<KnownKernel> table{{
        // ---- Official Arch kernels (core/extra repos) ----
        {.name                     = "linux",
            .display_name          = "Linux (mainline)",
            .description           = "Arch's default kernel — upstream stable releases; the general-purpose reference for desktops and "
                                     "servers.",
            .default_source        = "linux",
            .install_package       = "linux",
            .install_repo          = "core",
            .precompiled_available = true,
            .buildable             = true},
        {.name                     = "linux-lts",
            .display_name          = "Linux LTS",
            .description           = "Long-Term-Support kernel on the designated stable LTS branch — maximum stability and hardware "
                                     "compatibility.",
            .default_source        = "linux-lts",
            .install_package       = "linux-lts",
            .install_repo          = "core",
            .precompiled_available = true,
            .buildable             = true},
        {.name                     = "linux-zen",
            .display_name          = "Linux Zen",
            .description           = "Low-latency desktop kernel with the collaborative Zen patchset (tuned schedulers, memory, "
                                     "high-frequency timers) — a good daily driver.",
            .default_source        = "linux-zen",
            .install_package       = "linux-zen",
            .install_repo          = "extra",
            .precompiled_available = true,
            .buildable             = true},
        {.name                     = "linux-hardened",
            .display_name          = "Linux Hardened",
            .description           = "Security-hardened kernel with exploit-mitigation patches (dmesg restriction, strict RWX, page "
                                     "poisoning) for threat-aware users.",
            .default_source        = "linux-hardened",
            .install_package       = "linux-hardened",
            .install_repo          = "extra",
            .precompiled_available = true,
            .buildable             = true},
        {.name                     = "linux-rt",
            .display_name          = "Linux RT",
            .description           = "PREEMPT_RT real-time kernel (Molnar/Gleixner patchset) — deterministic, bounded latency for audio, "
                                     "automation and control workloads.",
            .default_source        = "linux-rt",
            .install_package       = "linux-rt",
            .install_repo          = "extra",
            .precompiled_available = true,
            .buildable             = true},
        {.name                     = "linux-rt-lts",
            .display_name          = "Linux RT LTS",
            .description           = "PREEMPT_RT on an LTS base — real-time determinism with long-term-support stability.",
            .default_source        = "linux-rt-lts",
            .install_package       = "linux-rt-lts",
            .install_repo          = "extra",
            .precompiled_available = true,
            .buildable             = true},
        // ---- CachyOS kernels (cachyos repo) ----
        {.name                     = "linux-cachyos",
            .display_name          = "Linux CachyOS",
            .description           = "CachyOS's heavily optimized mainline kernel — Clang ThinLTO, AutoFDO, Propeller, BORE scheduler; "
                                     "maximum desktop/gaming performance.",
            .default_source        = "linux-cachyos",
            .install_package       = "linux-cachyos",
            .install_repo          = "cachyos",
            .precompiled_available = true,
            .buildable             = true},
        {.name                     = "linux-cachyos-bore",
            .display_name          = "Linux CachyOS BORE",
            .description           = "Dedicated BORE (Burst-Oriented Response Enhancer) scheduler variant for interactive latency and frame "
                                     "pacing.",
            .default_source        = "linux-cachyos-bore",
            .install_package       = "linux-cachyos-bore",
            .install_repo          = "cachyos",
            .precompiled_available = true,
            .buildable             = true},
        {.name                     = "linux-cachyos-rt-bore",
            .display_name          = "Linux CachyOS RT BORE",
            .description           = "PREEMPT_RT + BORE — low-jitter real-time audio and simulation.",
            .default_source        = "linux-cachyos-rt-bore",
            .install_package       = "linux-cachyos-rt-bore",
            .install_repo          = "cachyos",
            .precompiled_available = true,
            .buildable             = true},
        {.name                     = "linux-cachyos-lts",
            .display_name          = "Linux CachyOS LTS",
            .description           = "LTS base with the CachyOS patchset — stable desktop with performance tuning.",
            .default_source        = "linux-cachyos-lts",
            .install_package       = "linux-cachyos-lts",
            .install_repo          = "cachyos",
            .precompiled_available = true,
            .buildable             = true},
        {.name                     = "linux-cachyos-server",
            .display_name          = "Linux CachyOS Server",
            .description           = "Server-oriented kernel — lazy preemption and server EEVDF tuning for high-concurrency and "
                                     "virtualization nodes.",
            .default_source        = "linux-cachyos-server",
            .install_package       = "linux-cachyos-server",
            .install_repo          = "cachyos",
            .precompiled_available = true,
            .buildable             = true},
        {.name                     = "linux-cachyos-deckify",
            .display_name          = "Linux CachyOS Deckify",
            .description           = "Optimized for handheld gaming consoles (Steam Deck, MSI Claw).",
            .default_source        = "linux-cachyos-deckify",
            .install_package       = "linux-cachyos-deckify",
            .install_repo          = "cachyos",
            .precompiled_available = true,
            .buildable             = true},
        {.name                     = "linux-cachyos-bmq",
            .display_name          = "Linux CachyOS BMQ",
            .description           = "BMQ (BitMap Queue) alternative-runqueue variant for scheduler benchmarking (no sched-ext).",
            .default_source        = "linux-cachyos-bmq",
            .install_package       = "linux-cachyos-bmq",
            .install_repo          = "cachyos",
            .precompiled_available = true,
            .buildable             = true},
        // ---- Community kernels (chaotic-aur / liquorix repos) ----
        {.name                     = "linux-mainline",
            .display_name          = "Linux Mainline",
            .description           = "Tracks Linus' master branch and weekly release candidates — newest hardware enablement, pre-stable.",
            .default_source        = "linux-mainline",
            .install_package       = "linux-mainline",
            .install_repo          = "chaotic-aur",
            .precompiled_available = true,
            .buildable             = true},
        {.name                     = "linux-tkg",
            .display_name          = "Linux TKG",
            .description           = "Frogging-Family's modular custom-kernel framework — user-selected scheduler (BORE/BMQ/PDS/EEVDF), "
                                     "compiler and patches; built from the tkg repo.",
            .default_source        = "https://github.com/Frogging-Family/linux-tkg.git",
            .install_package       = "",
            .install_repo          = "chaotic-aur",
            .precompiled_available = false,
            .buildable             = true},
        {.name                     = "linux-xanmod",
            .display_name          = "Linux XanMod",
            .description           = "Performance kernel — memory-allocation tuning, high-frequency ticks, BBRv3 and CAKE queueing; for "
                                     "multimedia and low latency.",
            .default_source        = "linux-xanmod",
            .install_package       = "linux-xanmod",
            .install_repo          = "chaotic-aur",
            .precompiled_available = true,
            .buildable             = true},
        {.name                     = "linux-xanmod-edge",
            .display_name          = "Linux XanMod Edge",
            .description           = "XanMod on a mainline (edge) base — experimental features with low-latency desktop tuning.",
            .default_source        = "linux-xanmod-edge",
            .install_package       = "linux-xanmod-edge",
            .install_repo          = "chaotic-aur",
            .precompiled_available = true,
            .buildable             = true},
        {.name                     = "linux-xanmod-lts",
            .display_name          = "Linux XanMod LTS",
            .description           = "XanMod on an LTS base — stable desktop with XanMod subsystems.",
            .default_source        = "linux-xanmod-lts",
            .install_package       = "linux-xanmod-lts",
            .install_repo          = "chaotic-aur",
            .precompiled_available = true,
            .buildable             = true},
        {.name                     = "linux-xanmod-rt",
            .display_name          = "Linux XanMod RT",
            .description           = "XanMod + PREEMPT_RT — deterministic processing with XanMod enhancements.",
            .default_source        = "linux-xanmod-rt",
            .install_package       = "linux-xanmod-rt",
            .install_repo          = "chaotic-aur",
            .precompiled_available = true,
            .buildable             = true},
        {.name                     = "linux-lqx",
            .display_name          = "Linux Liquorix",
            .description           = "Liquorix — Zen-based kernel tuned for desktop audio latency, multimedia and low-latency preemption.",
            .default_source        = "linux-lqx",
            .install_package       = "linux-lqx",
            .install_repo          = "liquorix",
            .precompiled_available = true,
            .buildable             = true},
        {.name                     = "linux-clear",
            .display_name          = "Linux Clear",
            .description           = "Intel Clear Linux performance and power-management patchset port — optimized for Intel platforms.",
            .default_source        = "linux-clear",
            .install_package       = "linux-clear",
            .install_repo          = "chaotic-aur",
            .precompiled_available = true,
            .buildable             = true},
    }};
    return table;
}

std::optional<const KnownKernel*> find_kernel(std::string_view name) {
    const auto key = kernel_name_from_raw(name);
    const auto it  = std::ranges::find_if(known_kernels(), [&](const auto& kernel) { return kernel.name == key; });
    if (it == known_kernels().end()) {
        return std::nullopt;
    }
    return &*it;
}

std::string default_source_for(std::string_view name) {
    const auto entry = find_kernel(name);
    if (entry.has_value()) {
        return (*entry)->default_source;
    }
    // Unknown kernel: build from its own package name so nothing is
    // unbuildable.
    return std::string{kernel_name_from_raw(name)};
}

std::string description_for(std::string_view name, std::string_view category) {
    const auto entry = find_kernel(name);
    if (entry.has_value()) {
        return (*entry)->description;
    }
    // Synthesized fallback line: "<Human Name> — <category> kernel
    // (no curated description yet)".
    std::string description = humanize_name(kernel_name_from_raw(name));
    if (!category.empty()) {
        description += " — ";
        description.append(category);
        description += " kernel";
    }
    description += " (no curated description yet)";
    return description;
}

std::vector<std::string> known_sources() {
    std::vector<std::string> sources{};
    sources.reserve(known_kernels().size());
    for (const auto& kernel : known_kernels()) {
        sources.emplace_back(kernel.default_source);
    }
    std::ranges::sort(sources);
    // Dedup adjacent equals (the sorted order guarantees duplicates are
    // adjacent) without the erase-unique idiom: std::ranges::unique's
    // return type is version-dependent (iterator in C++23, subrange in
    // C++26) and the idiom would not port across libstdc++/libc++ majors.
    std::vector<std::string> unique_sources{};
    unique_sources.reserve(sources.size());
    for (const auto& source : sources) {
        if (unique_sources.empty() || source != unique_sources.back()) {
            unique_sources.emplace_back(source);
        }
    }
    return unique_sources;
}

bool is_installable(std::string_view name) {
    const auto entry = find_kernel(name);
    if (!entry.has_value()) {
        return false;
    }
    const auto* kernel = *entry;
    return kernel->precompiled_available && !kernel->install_package.empty();
}

std::string_view kernel_name_from_raw(std::string_view raw) {
    if (const auto pos = raw.find('/'); pos != std::string_view::npos) {
        return raw.substr(pos + 1);
    }
    return raw;
}

}  // namespace km
