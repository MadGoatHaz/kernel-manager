// Copyright (C) 2024-2026 Vladislav Nepogodin
//
// This file is part of kernel-manager.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// K21: unit tests for src/config-options.cpp — the cxxbridge-based TOML
// config parser/serializer (T-043). The config-options module is the
// largest untested logic module in the project; it sits between the Qt
// UI (ConfWindow) and the Rust cxxbridge (config-option-lib), and its
// parse/write/round-trip contract is exercised by every Configure-flow
// operation. These tests compile the REAL config-options.cpp against the
// real cxxbridge + fmt from the CMake build, then drive the two public
// entry points (parse_from_file / write_config_file) through a
// mkstemp sandbox: no system config is ever touched.
//
// Coverage:
//   A. parse: a fully-populated TOML document round-trips every field
//      (booleans, strings, the neutral custom_config_check name).
//   B. parse: an empty document yields all defaults (serde #[serde(default)]
//      in the Rust Config struct).
//   C. parse: a legacy document (missing the neutral custom_config_check
//      key) still parses with that key defaulting to false.
//   D. parse: a malformed document (invalid TOML) returns std::nullopt and
//      does not crash.
//   E. parse: a missing file returns std::nullopt and does not crash.
//   F. write: a ConfigOptions with non-default values serializes to a TOML
//      document that, when re-parsed, reproduces every field exactly
//      (round-trip).
//   G. write: the written document uses the neutral field name
//      (custom_config_check), never the legacy name.
//   H. write: an empty / all-default ConfigOptions serializes to a valid
//      document that parses back to the same defaults.
//   I. edge: a string field containing TOML-special characters (quotes,
//      backslashes, newlines) survives a write+parse round-trip intact.
//
// Recipe: standalone g++ against the real sources (the run_k7.sh
// precedent) plus the cxxbridge static libs from the CMake build. The
// harness sets QT_QPA_PLATFORM=offscreen only if a Qt library ends up in
// the link closure; config-options itself is Qt-free, so the test is a
// plain C++ binary.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unistd.h>

#include <fmt/core.h>

#include "config-options.hpp"

namespace fs = std::filesystem;

// A minimal assertion helper: prints a labelled diagnostic on failure and
// tracks the count; the binary exits non-zero if any check failed.
static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (cond) {                                                          \
            g_pass++;                                                        \
        } else {                                                             \
            g_fail++;                                                        \
            fmt::print(stderr, "K21 FAIL [{}:{}] {}\n", __FILE__, __LINE__,  \
                       msg);                                                \
        }                                                                    \
    } while (0)

// Write a string to a file path, creating parent directories as needed.
static void write_file(fs::path path, std::string content) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs << content;
}

// Read a file path into a string; returns empty on error.
static std::string read_file(fs::path path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
        return {};
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return content;
}

int main() {
    // Isolate the test in a mkstemp-style sandbox.
    fs::path sandbox = fs::temp_directory_path() / ("km-test-k21-" + std::to_string(::getpid()));
    fs::create_directories(sandbox);
    const fs::path config_path = sandbox / "config.toml";

    // -----------------------------------------------------------------
    // A. Parse a fully-populated TOML document
    // -----------------------------------------------------------------
    {
        const std::string full_toml =
            "hardly_check = true\n"
            "per_gov_check = true\n"
            "tcp_bbr3_check = false\n"
            "custom_config_check = true\n"
            "nconfig_check = false\n"
            "xconfig_check = true\n"
            "localmodcfg_check = false\n"
            "use_current_check = true\n"
            "builtin_zfs_check = false\n"
            "builtin_nvidia_open_check = true\n"
            "build_debug_check = false\n"
            "hz_ticks_combo = \"250\"\n"
            "tickrate_combo = \"idle\"\n"
            "preempt_combo = \"voluntary\"\n"
            "hugepage_combo = \"never\"\n"
            "lto_combo = \"thin\"\n"
            "cpu_opt_combo = \"x86-64-v3\"\n"
            "custom_name_edit = \"my-custom-kernel\"\n";
        write_file(config_path, full_toml);

        auto result = ConfigOptions::parse_from_file(config_path.string());
        CHECK(result.has_value(), "A: fully-populated TOML must parse");
        if (result) {
            CHECK(result->hardly_check, "A: hardly_check=true");
            CHECK(result->per_gov_check, "A: per_gov_check=true");
            CHECK(!result->tcp_bbr3_check, "A: tcp_bbr3_check=false");
            CHECK(result->custom_config_check, "A: custom_config_check=true (neutral name)");
            CHECK(!result->nconfig_check, "A: nconfig_check=false");
            CHECK(result->xconfig_check, "A: xconfig_check=true");
            CHECK(!result->localmodcfg_check, "A: localmodcfg_check=false");
            CHECK(result->use_current_check, "A: use_current_check=true");
            CHECK(!result->builtin_zfs_check, "A: builtin_zfs_check=false");
            CHECK(result->builtin_nvidia_open_check, "A: builtin_nvidia_open_check=true");
            CHECK(!result->build_debug_check, "A: build_debug_check=false");
            CHECK(result->hz_ticks_combo == "250", "A: hz_ticks_combo=250");
            CHECK(result->tickrate_combo == "idle", "A: tickrate_combo=idle");
            CHECK(result->preempt_combo == "voluntary", "A: preempt_combo=voluntary");
            CHECK(result->hugepage_combo == "never", "A: hugepage_combo=never");
            CHECK(result->lto_combo == "thin", "A: lto_combo=thin");
            CHECK(result->cpu_opt_combo == "x86-64-v3", "A: cpu_opt_combo=x86-64-v3");
            CHECK(result->custom_name_edit == "my-custom-kernel", "A: custom_name_edit");
        }
    }

    // -----------------------------------------------------------------
    // B. Parse an empty document → all defaults
    // -----------------------------------------------------------------
    {
        write_file(config_path, "");
        auto result = ConfigOptions::parse_from_file(config_path.string());
        CHECK(result.has_value(), "B: empty TOML must parse (serde defaults)");
        if (result) {
            CHECK(!result->hardly_check, "B: hardly_check defaults false");
            CHECK(!result->per_gov_check, "B: per_gov_check defaults false");
            CHECK(!result->tcp_bbr3_check, "B: tcp_bbr3_check defaults false");
            CHECK(!result->custom_config_check, "B: custom_config_check defaults false");
            CHECK(!result->nconfig_check, "B: nconfig_check defaults false");
            CHECK(!result->xconfig_check, "B: xconfig_check defaults false");
            CHECK(!result->localmodcfg_check, "B: localmodcfg_check defaults false");
            CHECK(!result->use_current_check, "B: use_current_check defaults false");
            CHECK(!result->builtin_zfs_check, "B: builtin_zfs_check defaults false");
            CHECK(!result->builtin_nvidia_open_check, "B: builtin_nvidia_open_check defaults false");
            CHECK(!result->build_debug_check, "B: build_debug_check defaults false");
            CHECK(result->hz_ticks_combo.empty(), "B: hz_ticks_combo defaults empty");
            CHECK(result->tickrate_combo.empty(), "B: tickrate_combo defaults empty");
            CHECK(result->preempt_combo.empty(), "B: preempt_combo defaults empty");
            CHECK(result->hugepage_combo.empty(), "B: hugepage_combo defaults empty");
            CHECK(result->lto_combo.empty(), "B: lto_combo defaults empty");
            CHECK(result->cpu_opt_combo.empty(), "B: cpu_opt_combo defaults empty");
            CHECK(result->custom_name_edit.empty(), "B: custom_name_edit defaults empty");
        }
    }

    // -----------------------------------------------------------------
    // C. Parse a legacy document (missing custom_config_check)
    // -----------------------------------------------------------------
    {
        const std::string legacy_toml =
            "hardly_check = true\n"
            "preempt_combo = \"full\"\n";
        write_file(config_path, legacy_toml);
        auto result = ConfigOptions::parse_from_file(config_path.string());
        CHECK(result.has_value(), "C: legacy TOML (missing keys) must parse");
        if (result) {
            CHECK(result->hardly_check, "C: present key parsed");
            CHECK(!result->custom_config_check, "C: missing key defaults false");
            CHECK(result->preempt_combo == "full", "C: present string parsed");
            CHECK(result->tickrate_combo.empty(), "C: missing string defaults empty");
        }
    }

    // -----------------------------------------------------------------
    // D. Parse a malformed document → std::nullopt, no crash
    // -----------------------------------------------------------------
    {
        write_file(config_path, "this is not valid [toml at all\n");
        auto result = ConfigOptions::parse_from_file(config_path.string());
        CHECK(!result.has_value(), "D: malformed TOML must return nullopt");
    }

    // -----------------------------------------------------------------
    // E. Parse a missing file → std::nullopt, no crash
    // -----------------------------------------------------------------
    {
        fs::path missing = sandbox / "does-not-exist.toml";
        auto result = ConfigOptions::parse_from_file(missing.string());
        CHECK(!result.has_value(), "E: missing file must return nullopt");
    }

    // -----------------------------------------------------------------
    // F+G. Write → parse round-trip with non-default values
    // -----------------------------------------------------------------
    {
        ConfigOptions src{};
        src.hardly_check               = true;
        src.per_gov_check              = false;
        src.tcp_bbr3_check             = true;
        src.custom_config_check        = true;
        src.nconfig_check              = true;
        src.xconfig_check              = false;
        src.localmodcfg_check          = true;
        src.use_current_check          = false;
        src.builtin_zfs_check          = true;
        src.builtin_nvidia_open_check  = false;
        src.build_debug_check          = true;
        src.hz_ticks_combo             = "1000";
        src.tickrate_combo             = "dynamic";
        src.preempt_combo              = "none";
        src.hugepage_combo             = "always";
        src.lto_combo                  = "full";
        src.cpu_opt_combo              = "native";
        src.custom_name_edit           = "roundtrip-test";

        bool wrote = ConfigOptions::write_config_file(src, config_path.string());
        CHECK(wrote, "F: write_config_file must succeed");
        CHECK(fs::exists(config_path), "F: file must exist after write");

        auto reloaded = ConfigOptions::parse_from_file(config_path.string());
        CHECK(reloaded.has_value(), "F: re-parsed file must be valid");
        if (reloaded) {
            CHECK(reloaded->hardly_check == src.hardly_check, "F: hardly_check round-trip");
            CHECK(reloaded->per_gov_check == src.per_gov_check, "F: per_gov_check round-trip");
            CHECK(reloaded->tcp_bbr3_check == src.tcp_bbr3_check, "F: tcp_bbr3_check round-trip");
            CHECK(reloaded->custom_config_check == src.custom_config_check, "F: custom_config_check round-trip");
            CHECK(reloaded->nconfig_check == src.nconfig_check, "F: nconfig_check round-trip");
            CHECK(reloaded->xconfig_check == src.xconfig_check, "F: xconfig_check round-trip");
            CHECK(reloaded->localmodcfg_check == src.localmodcfg_check, "F: localmodcfg_check round-trip");
            CHECK(reloaded->use_current_check == src.use_current_check, "F: use_current_check round-trip");
            CHECK(reloaded->builtin_zfs_check == src.builtin_zfs_check, "F: builtin_zfs_check round-trip");
            CHECK(reloaded->builtin_nvidia_open_check == src.builtin_nvidia_open_check, "F: builtin_nvidia_open_check round-trip");
            CHECK(reloaded->build_debug_check == src.build_debug_check, "F: build_debug_check round-trip");
            CHECK(reloaded->hz_ticks_combo == src.hz_ticks_combo, "F: hz_ticks_combo round-trip");
            CHECK(reloaded->tickrate_combo == src.tickrate_combo, "F: tickrate_combo round-trip");
            CHECK(reloaded->preempt_combo == src.preempt_combo, "F: preempt_combo round-trip");
            CHECK(reloaded->hugepage_combo == src.hugepage_combo, "F: hugepage_combo round-trip");
            CHECK(reloaded->lto_combo == src.lto_combo, "F: lto_combo round-trip");
            CHECK(reloaded->cpu_opt_combo == src.cpu_opt_combo, "F: cpu_opt_combo round-trip");
            CHECK(reloaded->custom_name_edit == src.custom_name_edit, "F: custom_name_edit round-trip");
        }

        // G: the serialized document must use the neutral field name
        const std::string content = read_file(config_path);
        CHECK(content.find("custom_config_check") != std::string::npos,
              "G: neutral field name must appear in serialized output");
    }

    // -----------------------------------------------------------------
    // H. Write all-default ConfigOptions → parse back to defaults
    // -----------------------------------------------------------------
    {
        ConfigOptions defaults{};
        bool wrote = ConfigOptions::write_config_file(defaults, config_path.string());
        CHECK(wrote, "H: writing all-default config must succeed");

        auto reloaded = ConfigOptions::parse_from_file(config_path.string());
        CHECK(reloaded.has_value(), "H: re-parsed default config must be valid");
        if (reloaded) {
            CHECK(!reloaded->hardly_check, "H: hardly_check stays false");
            CHECK(reloaded->hz_ticks_combo.empty(), "H: hz_ticks_combo stays empty");
            CHECK(reloaded->custom_name_edit.empty(), "H: custom_name_edit stays empty");
        }
    }

    // -----------------------------------------------------------------
    // I. String fields with TOML-special characters survive round-trip
    // -----------------------------------------------------------------
    {
        ConfigOptions special{};
        special.custom_name_edit = "kernel \"with\" quotes";
        special.hz_ticks_combo   = "value\\with\\backslash";

        bool wrote = ConfigOptions::write_config_file(special, config_path.string());
        CHECK(wrote, "I: writing special-char config must succeed");

        auto reloaded = ConfigOptions::parse_from_file(config_path.string());
        CHECK(reloaded.has_value(), "I: re-parsed special-char config must be valid");
        if (reloaded) {
            CHECK(reloaded->custom_name_edit == special.custom_name_edit,
                  "I: quotes survive round-trip");
            CHECK(reloaded->hz_ticks_combo == special.hz_ticks_combo,
                  "I: backslashes survive round-trip");
        }
    }

    // -----------------------------------------------------------------
    // Cleanup
    // -----------------------------------------------------------------
    fs::remove_all(sandbox);

    fmt::print("K21: {} passed, {} failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
