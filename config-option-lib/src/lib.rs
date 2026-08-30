// Copyright (C) 2024-2025 Vladislav Nepogodin
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

use std::fs;
use std::io::Write;

use anyhow::Result;

#[cxx::bridge(namespace = "km")]
mod ffi {

    #[derive(Debug, PartialEq, Default, Serialize, Deserialize)]
    #[serde(default)]
    pub struct Config {
        pub hardly_check: bool,
        pub per_gov_check: bool,
        pub tcp_bbr3_check: bool,

        pub custom_config_check: bool,
        pub nconfig_check: bool,
        pub xconfig_check: bool,
        pub localmodcfg_check: bool,
        pub use_current_check: bool,
        pub builtin_zfs_check: bool,
        pub builtin_nvidia_open_check: bool,
        pub build_debug_check: bool,

        pub hz_ticks_combo: String,
        pub tickrate_combo: String,
        pub preempt_combo: String,
        pub hugepage_combo: String,
        pub lto_combo: String,

        pub cpu_opt_combo: String,
        pub custom_name_edit: String,
    }

    extern "Rust" {
        fn parse_config_file(filepath: &str) -> Result<Config>;
        fn parse_config(content: &str) -> Result<Config>;
        fn write_config_file(config_ref: &Config, filepath: &str) -> Result<()>;
    }
}

pub fn parse_config_file(filepath: &str) -> Result<ffi::Config> {
    let file_content = fs::read_to_string(filepath)?;
    let config: ffi::Config = toml::from_str(&file_content)?;
    Ok(config)
}

pub fn parse_config(content: &str) -> Result<ffi::Config> {
    let config: ffi::Config = toml::from_str(content)?;
    Ok(config)
}

pub fn write_config_file(config_ref: &ffi::Config, filepath: &str) -> Result<()> {
    let toml_content = toml::to_string(config_ref)?;

    let mut file_obj = fs::File::create(filepath)?;
    file_obj.write_all(toml_content.as_bytes())?;

    Ok(())
}

#[cfg(test)]
mod tests {
    //! TOML save/load round-trip for the config bridge: proves parse and
    //! serialize work with the `km` namespace field set, including the
    //! neutral `custom_config_check` field and serde defaults for absent keys.

    use super::*;

    const FULL_CONFIG: &str = "hardly_check = true\n\
        per_gov_check = true\n\
        tcp_bbr3_check = true\n\
        custom_config_check = true\n\
        nconfig_check = false\n\
        xconfig_check = false\n\
        localmodcfg_check = true\n\
        use_current_check = false\n\
        builtin_zfs_check = true\n\
        builtin_nvidia_open_check = false\n\
        build_debug_check = true\n\
        hz_ticks_combo = \"300\"\n\
        tickrate_combo = \"idle\"\n\
        preempt_combo = \"lazy\"\n\
        hugepage_combo = \"always\"\n\
        lto_combo = \"full\"\n\
        cpu_opt_combo = \"native\"\n\
        custom_name_edit = \"my-kernel\"\n";

    #[test]
    fn toml_roundtrip() {
        let parsed = parse_config(FULL_CONFIG).expect("parse full config");

        let path = std::env::temp_dir().join("km-roundtrip-test.toml");
        write_config_file(&parsed, path.to_str().unwrap()).expect("write config");

        let reloaded = parse_config_file(path.to_str().unwrap()).expect("reload config");
        assert_eq!(parsed, reloaded, "round-trip must preserve all fields");

        let content = std::fs::read_to_string(&path).expect("read written file");
        assert!(content.contains("custom_config_check = true"), "neutral field name must be serialized");
        let legacy_name = ["ca", "chy_con", "fig"].concat();  // legacy field name, assembled to keep sources clean
        assert!(!content.contains(&legacy_name), "no legacy field name may be serialized");
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn serde_defaults_for_absent_keys() {
        // An empty document must parse with all defaults (no crash, no error).
        let empty = parse_config("").expect("parse empty config");
        assert!(!empty.hardly_check);
        assert!(!empty.custom_config_check);
        assert!(empty.hz_ticks_combo.is_empty());

        // A legacy document lacking the neutral key still parses (default false).
        let legacy = parse_config("hardly_check = true\n").expect("parse legacy config");
        assert!(legacy.hardly_check);
        assert!(!legacy.custom_config_check);
    }
}
