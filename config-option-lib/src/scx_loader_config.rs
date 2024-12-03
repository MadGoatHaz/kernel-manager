// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2024 Vladislav Nepogodin <vnepogodin@cachyos.org>

// This software may be used and distributed according to the terms of the
// GNU General Public License version 2.

use crate::utils;

use std::collections::HashMap;
use std::fs;
use std::io::Write;
use std::path::Path;
use std::process::Command;

use anyhow::{Context, Result};
use serde::{Deserialize, Serialize};
use tokio::runtime::Runtime;
use zbus::Connection;

#[cxx::bridge(namespace = "scx_loader")]
mod ffi {
    extern "Rust" {
        type Config;

        /// Initialize config from config path, if the file doesn't exist config with default values
        /// will be created.
        fn init_config_file(config_path: &str) -> Result<Box<Config>>;

        /// Get the scx flags for the given sched mode
        fn get_scx_flags_for_mode(
            &self,
            supported_sched: &str,
            sched_mode: u32,
        ) -> Result<Vec<String>>;

        /// Applies the scx scheduler with arguments/mode
        fn apply_scheduler_change(
            &mut self,
            scx_name: &str,
            scx_mode: u32,
            extra_flags: &str,
            config_path: &str,
        ) -> Result<()>;

        /// Disables auto start of scheduler, and stops current scheduler
        fn disable_scheduler(&mut self, config_path: &str) -> Result<()>;
    }
}

// TODO(vnepogodin): instead of copying code from scx_loader, the scx_loader should be crate with
// provided functions and traits which we call here

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum SupportedSched {
    #[serde(rename = "scx_bpfland")]
    Bpfland,
    #[serde(rename = "scx_rusty")]
    Rusty,
    #[serde(rename = "scx_lavd")]
    Lavd,
    #[serde(rename = "scx_flash")]
    Flash,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub enum SchedMode {
    /// Default values for the scheduler
    Auto = 0,
    /// Applies flags for better gaming experience
    Gaming = 1,
    /// Applies flags for lower power usage
    PowerSave = 2,
    /// Starts scheduler in low latency mode
    LowLatency = 3,
}

impl TryFrom<u32> for SchedMode {
    type Error = anyhow::Error;

    fn try_from(value: u32) -> Result<Self> {
        match value {
            0 => Ok(SchedMode::Auto),
            1 => Ok(SchedMode::Gaming),
            2 => Ok(SchedMode::PowerSave),
            3 => Ok(SchedMode::LowLatency),
            _ => anyhow::bail!("SchedMode with such value doesn't exist"),
        }
    }
}

#[derive(Debug, PartialEq, Default, Serialize, Deserialize)]
#[serde(default)]
pub struct Config {
    pub default_sched: Option<SupportedSched>,
    pub default_mode: Option<SchedMode>,
    pub scheds: HashMap<String, Sched>,
}

#[derive(Debug, PartialEq, Serialize, Deserialize)]
pub struct Sched {
    pub auto_mode: Option<Vec<String>>,
    pub gaming_mode: Option<Vec<String>>,
    pub lowlatency_mode: Option<Vec<String>>,
    pub powersave_mode: Option<Vec<String>>,
}

fn init_config_file(config_path: &str) -> Result<Box<Config>> {
    let config = init_config(config_path).context("Failed to initialize config")?;
    Ok(Box::new(config))
}

#[zbus::proxy(
    interface = "org.scx.Loader",
    default_service = "org.scx.Loader",
    default_path = "/org/scx/Loader"
)]
pub trait LoaderClient {
    /// Method for switching to the specified scheduler with the provided arguments.
    /// This method will stop the currently running scheduler (if any) and
    /// then start the new scheduler with the given arguments.
    fn switch_scheduler_with_args(&self, scx_name: &str, scx_args: &[String]) -> zbus::Result<()>;

    /// Stops the currently running scheduler.
    fn stop_scheduler(&self) -> zbus::Result<()>;
}

impl Config {
    /// Write the config to the file.
    fn write_config_file(&self, filepath: &str) -> Result<()> {
        let toml_content = toml::to_string(self)?;

        let mut file_obj = fs::File::create(filepath)?;
        file_obj.write_all(toml_content.as_bytes())?;

        Ok(())
    }

    fn get_scx_flags_for_mode(
        &self,
        supported_sched: &str,
        sched_mode: u32,
    ) -> Result<Vec<String>> {
        let scx_sched = get_scx_from_str(supported_sched)?;
        let sched_mode: SchedMode = sched_mode.try_into()?;
        let args = get_scx_flags_for_mode(self, &scx_sched, sched_mode);
        Ok(args)
    }

    /// Set the default scheduler with default mode
    fn set_scx_sched_with_mode(
        &mut self,
        scx_sched: SupportedSched,
        sched_mode: SchedMode,
    ) -> Result<()> {
        self.default_sched = Some(scx_sched);
        self.default_mode = Some(sched_mode);

        Ok(())
    }

    /// Disables auto start of scheduler, and stops current scheduler
    fn disable_scx_sched(&mut self, config_path: &str) -> Result<()> {
        // for us to correctly disable it:
        // 1. set `default_sched` to None
        // 2. edit config file
        // 3. call method to stop current scheduler

        // 1.
        self.default_sched = None;

        // 2.
        self.write_config_file(config_path).context("Failed to edit config file")?;

        // 3.
        let rt = Runtime::new().context("Failed to initialize tokio runtime")?;
        rt.block_on(async move {
            let connection = Connection::system().await?;
            let loader_client = LoaderClientProxy::new(&connection).await?;
            loader_client.stop_scheduler().await?;

            anyhow::Ok(())
        })?;

        Ok(())
    }

    fn switch_scheduler_with_args(&self, scx_name: &str, scx_args: &[String]) -> Result<()> {
        let rt = Runtime::new().context("Failed to initialize tokio runtime")?;
        rt.block_on(async move {
            let connection = Connection::system().await?;
            let loader_client = LoaderClientProxy::new(&connection).await?;
            loader_client.switch_scheduler_with_args(scx_name, scx_args).await?;

            anyhow::Ok(())
        })?;

        Ok(())
    }

    fn apply_scheduler_change(
        &mut self,
        scx_name: &str,
        scx_mode: u32,
        extra_flags: &str,
        config_path: &str,
    ) -> Result<()> {
        // stop/disable 'scx.service' if its running/enabled on the system,
        // overwise it will conflict
        disable_scx_service();

        let scx_sched = get_scx_from_str(scx_name)?;
        let scx_mode: SchedMode = scx_mode.try_into()?;

        let mut sched_args: Vec<String> = vec![];
        if !extra_flags.is_empty() {
            // NOTE: should be Vec instead of str
            let mut extra_args: Vec<_> = extra_flags.split(' ').map(String::from).collect();
            sched_args.append(&mut extra_args);
        }

        println!("Applying scx '{scx_name}' with args: {}", sched_args.join(" ").to_owned());
        if let Err(scx_err) = self.switch_scheduler_with_args(scx_name, &sched_args) {
            println!("Failed to switch '{scx_name}' with args: {sched_args:?}: {scx_err}");
        }

        // enable scx_loader service if not enabled yet, it fully replaces scx.service
        if !is_scx_loader_service_enabled() {
            println!("Enabling scx_loader service");
            spawn_child_process("/usr/bin/systemctl", &["enable", "-f", "scx_loader"]);
        }

        // change default scheduler and default scheduler mode
        self.set_scx_sched_with_mode(scx_sched, scx_mode)
            .context("Cannot set default scx scheduler with mode")?;

        // write scx_loader configuration to the temp file
        let tmp_config_path = "/tmp/scx_loader.toml";
        self.write_config_file(tmp_config_path)
            .context("Cannot write scx_loader config to file")?;

        // copy scx_loader configuration from the temp file to the actual path with root permissions
        spawn_child_process("/usr/bin/pkexec", &["/usr/bin/cp", tmp_config_path, config_path]);

        Ok(())
    }

    fn disable_scheduler(&mut self, config_path: &str) -> Result<()> {
        // write scx_loader configuration to the temp file
        let tmp_config_path = "/tmp/scx_loader.toml";
        self.disable_scx_sched(tmp_config_path).context("Cannot disable scx_loader")?;

        // copy scx_loader configuration from the temp file to the actual path with root permissions
        spawn_child_process("/usr/bin/pkexec", &["/usr/bin/cp", tmp_config_path, config_path]);

        Ok(())
    }
}

/// Initialize config from first found config path, overwise fallback to default config
pub fn init_config(config_path: &str) -> Result<Config> {
    if Path::new(config_path).exists() {
        parse_config_file(config_path)
    } else {
        Ok(get_default_config())
    }
}

pub fn parse_config_file(filepath: &str) -> Result<Config> {
    let file_content = fs::read_to_string(filepath)?;
    parse_config_content(&file_content)
}

fn parse_config_content(file_content: &str) -> Result<Config> {
    if file_content.is_empty() {
        anyhow::bail!("The config file is empty!")
    }
    let config: Config = toml::from_str(file_content)?;
    Ok(config)
}

pub fn get_default_config() -> Config {
    Config {
        default_sched: None,
        default_mode: Some(SchedMode::Auto),
        scheds: HashMap::from([
            ("scx_bpfland".to_string(), get_default_sched_for_config(&SupportedSched::Bpfland)),
            ("scx_rusty".to_string(), get_default_sched_for_config(&SupportedSched::Rusty)),
            ("scx_lavd".to_string(), get_default_sched_for_config(&SupportedSched::Lavd)),
            ("scx_flash".to_string(), get_default_sched_for_config(&SupportedSched::Flash)),
        ]),
    }
}

/// Get the scx flags for the given sched mode
pub fn get_scx_flags_for_mode(
    config: &Config,
    scx_sched: &SupportedSched,
    sched_mode: SchedMode,
) -> Vec<String> {
    if let Some(sched_config) = config.scheds.get(get_name_from_scx(scx_sched)) {
        let scx_flags = extract_scx_flags_from_config(sched_config, &sched_mode);

        // try to exact flags from config, otherwise fallback to hardcoded default
        scx_flags.unwrap_or({
            get_default_scx_flags_for_mode(scx_sched, sched_mode)
                .into_iter()
                .map(String::from)
                .collect()
        })
    } else {
        get_default_scx_flags_for_mode(scx_sched, sched_mode)
            .into_iter()
            .map(String::from)
            .collect()
    }
}

/// Extract the scx flags from config
fn extract_scx_flags_from_config(
    sched_config: &Sched,
    sched_mode: &SchedMode,
) -> Option<Vec<String>> {
    match sched_mode {
        SchedMode::Gaming => sched_config.gaming_mode.clone(),
        SchedMode::LowLatency => sched_config.lowlatency_mode.clone(),
        SchedMode::PowerSave => sched_config.powersave_mode.clone(),
        SchedMode::Auto => sched_config.auto_mode.clone(),
    }
}

/// Get Sched object for configuration object
fn get_default_sched_for_config(scx_sched: &SupportedSched) -> Sched {
    Sched {
        auto_mode: Some(
            get_default_scx_flags_for_mode(scx_sched, SchedMode::Auto)
                .into_iter()
                .map(String::from)
                .collect(),
        ),
        gaming_mode: Some(
            get_default_scx_flags_for_mode(scx_sched, SchedMode::Gaming)
                .into_iter()
                .map(String::from)
                .collect(),
        ),
        lowlatency_mode: Some(
            get_default_scx_flags_for_mode(scx_sched, SchedMode::LowLatency)
                .into_iter()
                .map(String::from)
                .collect(),
        ),
        powersave_mode: Some(
            get_default_scx_flags_for_mode(scx_sched, SchedMode::PowerSave)
                .into_iter()
                .map(String::from)
                .collect(),
        ),
    }
}

/// Get the default scx flags for the given sched mode
fn get_default_scx_flags_for_mode(scx_sched: &SupportedSched, sched_mode: SchedMode) -> Vec<&str> {
    match scx_sched {
        SupportedSched::Bpfland => match sched_mode {
            SchedMode::Gaming => vec!["-m", "performance"],
            SchedMode::LowLatency => vec!["-k", "-s", "5000", "-l", "5000"],
            SchedMode::PowerSave => vec!["-m", "powersave"],
            SchedMode::Auto => vec![],
        },
        SupportedSched::Lavd => match sched_mode {
            SchedMode::Gaming | SchedMode::LowLatency => vec!["--performance"],
            SchedMode::PowerSave => vec!["--powersave"],
            // NOTE: potentially adding --auto in future
            SchedMode::Auto => vec![],
        },
        // scx_rusty doesn't support any of these modes
        SupportedSched::Rusty => vec![],
        // scx_flash doesn't support any of these modes
        SupportedSched::Flash => vec![],
    }
}

/// Get the scx trait from the given scx name or return error if the given scx name is not supported
fn get_scx_from_str(scx_name: &str) -> Result<SupportedSched> {
    match scx_name {
        "scx_bpfland" => Ok(SupportedSched::Bpfland),
        "scx_rusty" => Ok(SupportedSched::Rusty),
        "scx_lavd" => Ok(SupportedSched::Lavd),
        "scx_flash" => Ok(SupportedSched::Flash),
        _ => Err(anyhow::anyhow!("{scx_name} is not supported")),
    }
}

/// Get the scx name from the given scx trait
fn get_name_from_scx(supported_sched: &SupportedSched) -> &'static str {
    match supported_sched {
        SupportedSched::Bpfland => "scx_bpfland",
        SupportedSched::Rusty => "scx_rusty",
        SupportedSched::Lavd => "scx_lavd",
        SupportedSched::Flash => "scx_flash",
    }
}

fn spawn_child_process(cmd: &str, args: &[&str]) {
    let status = Command::new(cmd).args(args).status().expect("failed to execute child process");

    if !status.success() {
        println!("child process failed with exit code: {status:?}");
    }
}

fn is_scx_loader_service_enabled() -> bool {
    return utils::exec("systemctl is-enabled scx_loader") == "enabled";
}

fn is_scx_service_enabled() -> bool {
    return utils::exec("systemctl is-enabled scx") == "enabled";
}

fn is_scx_service_active() -> bool {
    return utils::exec("systemctl is-active scx") == "active";
}

fn disable_scx_service() {
    if is_scx_service_enabled() {
        spawn_child_process("/usr/bin/systemctl", &["disable", "--now", "-f", "scx"]);
        println!("Disabling scx service");
    } else if is_scx_service_active() {
        spawn_child_process("/usr/bin/systemctl", &["stop", "-f", "scx"]);
        println!("Stoping scx service");
    }
}

#[cfg(test)]
mod tests {
    use crate::scx_loader_config::*;

    #[test]
    fn test_default_config() {
        let config_str = r#"
default_mode = "Auto"

[scheds.scx_bpfland]
auto_mode = []
gaming_mode = ["-m", "performance"]
lowlatency_mode = ["-k", "-s", "5000", "-l", "5000"]
powersave_mode = ["-m", "powersave"]

[scheds.scx_rusty]
auto_mode = []
gaming_mode = []
lowlatency_mode = []
powersave_mode = []

[scheds.scx_lavd]
auto_mode = []
gaming_mode = ["--performance"]
lowlatency_mode = ["--performance"]
powersave_mode = ["--powersave"]

[scheds.scx_flash]
auto_mode = []
gaming_mode = []
lowlatency_mode = []
powersave_mode = []
"#;

        let parsed_config = parse_config_content(config_str).expect("Failed to parse config");
        let expected_config = get_default_config();

        assert_eq!(parsed_config, expected_config);
    }

    #[test]
    fn test_simple_fallback_config_flags() {
        let config_str = r#"
default_mode = "Auto"
"#;

        let parsed_config = parse_config_content(config_str).expect("Failed to parse config");

        let bpfland_flags =
            get_scx_flags_for_mode(&parsed_config, &SupportedSched::Bpfland, SchedMode::Gaming);
        let expected_flags =
            get_default_scx_flags_for_mode(&SupportedSched::Bpfland, SchedMode::Gaming);
        assert_eq!(bpfland_flags.iter().map(|x| x.as_str()).collect::<Vec<&str>>(), expected_flags);
    }

    #[test]
    fn test_sched_fallback_config_flags() {
        let config_str = r#"
default_mode = "Auto"

[scheds.scx_lavd]
auto_mode = ["--help"]
"#;

        let parsed_config = parse_config_content(config_str).expect("Failed to parse config");

        let lavd_flags =
            get_scx_flags_for_mode(&parsed_config, &SupportedSched::Lavd, SchedMode::Gaming);
        let expected_flags =
            get_default_scx_flags_for_mode(&SupportedSched::Lavd, SchedMode::Gaming);
        assert_eq!(lavd_flags.iter().map(|x| x.as_str()).collect::<Vec<&str>>(), expected_flags);

        let lavd_flags =
            get_scx_flags_for_mode(&parsed_config, &SupportedSched::Lavd, SchedMode::Auto);
        assert_eq!(lavd_flags.iter().map(|x| x.as_str()).collect::<Vec<&str>>(), vec!["--help"]);
    }

    #[test]
    fn test_empty_config() {
        let config_str = "";
        let result = parse_config_content(config_str);
        assert!(result.is_err());
    }
}
