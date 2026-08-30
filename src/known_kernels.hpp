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

#ifndef KNOWN_KERNELS_HPP_
#define KNOWN_KERNELS_HPP_

#include <optional>     // for optional
#include <string>       // for string, string_view
#include <string_view>  // for string_view
#include <vector>       // for vector

// The curated table of known kernel packages: the single source of truth for
// the kernel -> build-source mapping (Configure-page source dropdown +
// auto-populate) and for the per-kernel descriptions (kernel-tree tooltips).
//
// Lookup key = the package name with the repo prefix stripped from the tree
// PkgName ("core/linux" -> "linux", "aur/linux-zen" -> "linux-zen"); all
// lookup helpers accept either form (a "repo/" prefix is stripped internally).
//
// Adjusting the mapping (e.g. which source a kernel builds from) is a data
// change — edit one table row in known_kernels.cpp, no control flow involved.
struct KnownKernel {
    std::string name;            // package name, repo prefix stripped, e.g. "linux"
    std::string display_name;    // human label, e.g. "Linux (mainline)"
    std::string description;     // 1-2 sentences: what it is, who it's for
    std::string default_source;  // build-source identifier (AUR name or git URL)
};

namespace km {

// The full curated table (stable, persistent storage; references to entries
// remain valid for the process lifetime).
const std::vector<KnownKernel>& known_kernels();

// Find the table entry for a kernel name (a "repo/" prefix is stripped
// first); nullopt when the kernel is not curated.
std::optional<const KnownKernel*> find_kernel(std::string_view name);

// The default build source for a kernel name: the curated mapping, or the
// name itself as a fallback so no kernel becomes unbuildable.
std::string default_source_for(std::string_view name);

// A 1-2 sentence description for a kernel name: the curated entry, or a
// synthesized "<Human Name> — <category> kernel" line for unknown names
// (category is the Kernel::category() string; may be empty).
std::string description_for(std::string_view name, std::string_view category = {});

// The known build sources for the source dropdown: the unique default_source
// values of the curated table, sorted.
std::vector<std::string> known_sources();

// The package name with a leading "repo/" prefix stripped
// ("core/linux" -> "linux"; "linux" -> "linux").
[[gnu::pure]] std::string_view kernel_name_from_raw(std::string_view raw);

}  // namespace km

#endif  // KNOWN_KERNELS_HPP_
