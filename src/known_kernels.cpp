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

#include <algorithm>   // for unique
#include <ranges>      // for ranges::find_if, sort
#include <string_view> // for string_view

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
        {"linux",
         "Linux (mainline)",
         "Arch's default kernel — upstream stable releases; the general-purpose reference for desktops and "
             "servers.",
         "linux",
         "linux",
         "core",
         true,
         true},
        {"linux-zen",
         "Linux Zen",
         "Tuned for desktop responsiveness and low latency — a good general-purpose daily-driver kernel.",
         "linux-zen",
         "linux-zen",
         "extra",
         true,
         true},
        {"linux-lts",
         "Linux LTS",
         "Long-Term-Support kernel — an older, stable branch maintained for years; best for stability and "
             "hardware compatibility.",
         "linux-lts",
         "linux-lts",
         "core",
         true,
         true},
        {"linux-cachyos",
         "Linux CachyOS",
         "CachyOS's heavily-optimized mainline kernel (BPF, scheduler, and compiler tuning) — aimed at maximum "
             "performance.",
         "linux-cachyos",
         "linux-cachyos",
         "cachyos",
         true,
         true},
        {"linux-hardened",
         "Linux Hardened",
         "Security-hardened kernel with extra exploit-mitigation patches — for security-focused users.",
         "linux-hardened",
         "linux-hardened",
         "extra",
         true,
         true},
        {"linux-rt",
         "Linux RT",
         "Real-time (PREEMPT_RT) patchset — for low, predictable latency in audio/automation/control "
             "workloads.",
         "linux-rt",
         "linux-rt",
         "extra",
         true,
         true},
    }};
    return table;
}

std::optional<const KnownKernel*> find_kernel(std::string_view name) {
    const auto key = kernel_name_from_raw(name);
    const auto it = std::ranges::find_if(known_kernels(), [&](const auto& kernel) { return kernel.name == key; });
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
        sources.push_back(kernel.default_source);
    }
    std::ranges::sort(sources);
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
    return sources;
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
