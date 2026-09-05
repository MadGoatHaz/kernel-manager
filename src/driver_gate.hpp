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

#ifndef DRIVER_GATE_HPP
#define DRIVER_GATE_HPP

#include <cstdint>      // for uint8_t
#include <functional>   // for function
#include <string>       // for string
#include <string_view>  // for string_view

// The Qt-free nvidia driver packaging gate: the DKMS-vs-prebuilt decision
// made BEFORE any kernel build/install (plan D1-D3, D6, D7). The detection
// facts come from the local package DB, the PCI vendor state, and lspci —
// the same sources of truth the existing nvidia branch in kernel.cpp uses,
// but read-only and pre-flight: the gate never installs anything itself
// (the command payloads in the verdict are executed by the Qt layer).
namespace driver_gate {

// The GPU generation class (D2): the open kernel module (nvidia-open-dkms)
// supports Turing-and-newer GPUs (Volta included); pre-Turing cards
// (Fermi to Pascal) require the proprietary module (nvidia-dkms).
// UNKNOWN = undeterminable (no nvidia lspci line, lspci absent, ...) and
// select_dkms_package() falls back to it conservatively.
enum class GpuGeneration : std::uint8_t { TURING_PLUS,
    PRE_TURING,
    UNKNOWN };

// The gate outcome (the D3 decision table): PROCEED = nothing to gate (no
// nvidia hardware, no nvidia driver, or the DKMS driver + headers are in
// place); WARN_MIGRATE = a precompiled driver is installed and the DKMS
// variant should replace/consolidate it (one-click migration payload);
// ENSURE_HEADERS = the DKMS driver is installed but the target kernel's
// headers are missing and installable from a sync repo; WARN_ONLY = the
// headers are missing and not in any sync repo (no fix exists — the warning
// + the persistent D7 banner, proceed/cancel is the user's call).
enum class GateAction : std::uint8_t { PROCEED,
    WARN_MIGRATE,
    ENSURE_HEADERS,
    WARN_ONLY };

// The install the gate is protecting: the target kernel package name ("" =
// none known) and its release string for the build-dir check ("" = unknown
// or a fresh flavor that has no build dir yet — the pairing provides it).
struct GateTarget {
    std::string kernel;
    std::string kver;
};

// Injection point (the DistroProbe precedent in distro.hpp): all system
// state behind predicates, so the pure decision is testable without a
// filesystem. An unbound predicate contributes no signal (false/""), never
// a crash.
struct GateProbe {
    std::function<bool()> nvidia_hardware;                    // /sys vendor 0x10de walk
    std::function<std::string()> gpu_names;                   // lspci -nn nvidia lines ("" if none)
    std::function<bool(std::string_view)> package_installed;  // local-DB dir membership
    std::function<bool(std::string_view)> build_dir_exists;   // /usr/lib/modules/<kver>/build
    std::function<bool(std::string_view)> repo_available;     // pacman -Siq membership
};

// The gate decision: the action + the plain-language message (the tr()
// source strings live here; the Qt layer wraps them for display) + the D7
// banner text ("" unless a decline path shows it) + the command payloads
// (dkms_package/migration_cmd for WARN_MIGRATE, headers_package for every
// non-PROCEED row, ensure_cmd for ENSURE_HEADERS — "" otherwise).
struct GateVerdict {
    GateAction action = GateAction::PROCEED;
    std::string message;
    std::string banner;
    std::string dkms_package;
    std::string headers_package;
    std::string migration_cmd;
    std::string ensure_cmd;
};

// Derive the companion -headers package for a kernel (D3): append
// "-headers" — generic, NOT hardcoded to one flavor (linux-zen ->
// linux-zen-headers, linux-cachyos-custom -> linux-cachyos-custom-headers,
// linux -> linux-headers); idempotent (an input already ending in
// "-headers" is returned unchanged); empty input -> empty.
[[nodiscard]] std::string derive_headers_pkg(std::string_view kernel);

// Classify one raw lspci line (D2): a case-insensitive substring scan
// against the two curated disjoint marketing-name keyword tables — a
// TURING_PLUS hit -> TURING_PLUS, else a PRE_TURING hit -> PRE_TURING,
// else UNKNOWN. Chip codenames are not usable (Pascal and Turing share
// the GP10x family), so only the marketing tokens disambiguate.
[[nodiscard]] GpuGeneration classify_gpu(std::string_view lspci_line);

// The DKMS package for a generation (D2): TURING_PLUS -> "nvidia-open-dkms"
// (the recommended path on modern GPUs); PRE_TURING -> "nvidia-dkms" (the
// open module does not support pre-Turing); UNKNOWN -> "nvidia-dkms"
// (conservative — the proprietary module has the broadest GPU coverage; a
// false "open" is never selected on an unknown GPU).
[[nodiscard]] std::string select_dkms_package(GpuGeneration gen);

// The full D3 decision table over the injected probe (pure — no
// filesystem, no shell): returns the verdict with its message, banner,
// and command payloads for the given target.
[[nodiscard]] GateVerdict evaluate_gate(const GateProbe& probe, const GateTarget& target);

// Real (wired to disk — the distro.hpp "real overload" precedent):
[[nodiscard]] GateVerdict evaluate_gate(const GateTarget& target);
[[nodiscard]] bool nvidia_hardware_present();                  // /sys/bus/pci devices with vendor 0x10de
[[nodiscard]] bool package_installed(std::string_view name);   // /var/lib/pacman/local/<name>-<version>/
[[nodiscard]] bool dkms_driver_installed();                    // nvidia-dkms || nvidia-open-dkms
[[nodiscard]] bool precompiled_driver_installed();             // the 4 precompiled family names
[[nodiscard]] std::string gpu_names();                         // the nvidia lspci lines ("" if none)
[[nodiscard]] bool build_dir_exists(std::string_view kver);    // /usr/lib/modules/<kver>/build
[[nodiscard]] bool package_in_sync_db(std::string_view name);  // pacman -Siq non-empty (D6)
[[nodiscard]] std::string dkms_status();                       // best-effort "dkms status" ("" if absent)

}  // namespace driver_gate

#endif  // DRIVER_GATE_HPP
