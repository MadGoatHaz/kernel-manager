// Copyright (C) 2022-2026 Vladislav Nepogodin
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

#include "conf-window.hpp"
#include "compile_options.hpp"
#include "config-options.hpp"
#include "known_kernels.hpp"
#include "utils.hpp"

#include <cstdio>
#include <cstdlib>

#include <algorithm>    // for for_each, transform
#include <filesystem>   // for permissions
#include <optional>     // for optional
#include <ranges>       // for ranges::*
#include <string_view>  // for string_view

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnull-dereference"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wsuggest-final-types"
#pragma GCC diagnostic ignored "-Wsuggest-attribute=pure"
#pragma GCC diagnostic ignored "-Wconversion"
#endif

#include <QComboBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStringList>

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <fmt/compile.h>
#include <fmt/core.h>

namespace fs = std::filesystem;

// NOLINTBEGIN(cppcoreguidelines-macro-usage)

/**
 * GENERATE_CONST_OPTION_VALUES(name, ...):
 *
 * Used to define constant values for options.
 */
#define GENERATE_CONST_OPTION_VALUES(name, ...)                             \
    [[gnu::pure]] constexpr const char* get_##name(size_t index) noexcept { \
        constexpr std::array list_##name{__VA_ARGS__};                      \
        return list_##name[index];                                          \
    }

/**
 * GENERATE_CONST_LOOKUP_VALUES(name, ...):
 *
 * Used to define lookup values of option.
 */
#define GENERATE_CONST_LOOKUP_VALUES(name, ...)                                       \
    [[gnu::pure]] constexpr ssize_t lookup_##name(std::string_view needle) noexcept { \
        constexpr std::array list_##name{__VA_ARGS__};                                \
        for (size_t i = 0; i < list_##name.size(); ++i) {                             \
            if (std::string_view{list_##name[i]} == needle) {                         \
                return static_cast<ssize_t>(i);                                       \
            }                                                                         \
        }                                                                             \
        return -1;                                                                    \
    }

/**
 * GENERATE_CONST_LOOKUP_OPTION_VALUES(name, ...):
 *
 * Generates both values lookup and const values functions.
 */
#define GENERATE_CONST_LOOKUP_OPTION_VALUES(name, ...) \
    GENERATE_CONST_OPTION_VALUES(name, __VA_ARGS__)    \
    GENERATE_CONST_LOOKUP_VALUES(name, __VA_ARGS__)

namespace {

GENERATE_CONST_LOOKUP_OPTION_VALUES(hz_tick, "1000", "750", "600", "500", "300", "250", "100")
GENERATE_CONST_LOOKUP_OPTION_VALUES(tickless_mode, "full", "idle", "periodic")
GENERATE_CONST_LOOKUP_OPTION_VALUES(preempt_mode, "full", "lazy", "voluntary", "none")
GENERATE_CONST_LOOKUP_OPTION_VALUES(lto_mode, "none", "full", "thin", "thin-dist")
GENERATE_CONST_LOOKUP_OPTION_VALUES(hugepage_mode, "always", "madvise")
GENERATE_CONST_LOOKUP_OPTION_VALUES(cpu_opt_mode, "manual", "native", "generic_v1", "generic_v2", "generic_v3", "generic_v4", "zen4")

// NOLINTEND(cppcoreguidelines-macro-usage)

// Neutral display label for a discovered flavor key: "Default" for the base
// flavor, otherwise the humanized key ("rt-bore" -> "Rt Bore").
auto flavor_display_name(const std::string& key, const std::string& base_key) noexcept -> std::string {
    if (key == base_key) {
        return "Default";
    }
    std::string label{};
    bool capitalize_next{true};
    for (const char c : key) {
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

// Discover the flavors offered by the kernel source repo: every subdirectory
// containing a PKGBUILD is one flavor. Keys are derived from the directory
// names relative to their common family prefix, so no flavor names are
// hard-coded here. A single-package layout (a PKGBUILD at the repo root,
// e.g. an AUR repo) yields the repo itself as its sole flavor. Paths are
// always absolute (resolved against repo_root) so downstream consumers can
// rely on them regardless of the process CWD.
auto discover_repo_flavors(const fs::path& repo_root, std::string_view fallback_repo_name) noexcept -> std::vector<KernelFlavor> {
    std::vector<std::string> dirs{};
    std::string single_package_dir{};  // set only for the single-package fallback below
    std::error_code ec{};
    if (fs::is_directory(repo_root, ec)) {
        for (const auto& entry : fs::directory_iterator(repo_root, ec)) {
            if (entry.is_directory(ec) && fs::is_regular_file(entry.path() / "PKGBUILD", ec)) {
                dirs.push_back(entry.path().filename().string());
            }
        }
    }
    if (dirs.empty() && !fallback_repo_name.empty() && fs::is_regular_file(repo_root / "PKGBUILD", ec)) {
        single_package_dir = std::string{fallback_repo_name};
        dirs.push_back(single_package_dir);
    }
    std::sort(dirs.begin(), dirs.end());
    if (dirs.empty()) {
        return {};
    }

    // Longest common prefix of all flavor dirs = the kernel family.
    std::string family{dirs.front()};
    for (const auto& dir : dirs) {
        std::size_t i = 0;
        while (i < family.size() && i < dir.size() && family[i] == dir[i]) {
            ++i;
        }
        family = family.substr(0, i);
        if (family.empty()) {
            break;
        }
    }
    while (family.ends_with('-')) {
        family.pop_back();
    }

    // Base flavor key = the family minus the generic "linux" base.
    std::string base_key = family;
    if (base_key.starts_with("linux-")) {
        base_key = base_key.substr(6);
    }
    if (base_key.empty()) {
        base_key = family;
    }

    std::vector<KernelFlavor> flavors{};
    flavors.reserve(dirs.size());
    for (const auto& dir : dirs) {
        KernelFlavor flavor{};
        // Absolute path: a subdirectory flavor lives under the repo root,
        // while the single-package layout (fallback repo basename) IS the
        // repo root itself.
        flavor.path = (dir == single_package_dir) ? repo_root.string() : (repo_root / dir).string();
        flavor.key  = (dir == family) ? base_key : dir.substr(family.size() + 1);
        flavors.emplace_back(std::move(flavor));
    }
    return flavors;
}

inline bool checkstate_checked(QCheckBox* checkbox) noexcept {
    return (checkbox->checkState() == Qt::Checked);
}

inline void set_checkstate(QCheckBox* checkbox, bool is_checked) noexcept {
    checkbox->setCheckState(is_checked ? Qt::Checked : Qt::Unchecked);
}

struct CheckboxBinding {
    QCheckBox* Ui::ConfOptionsPage::* widget;
    bool ConfigOptions::* config_field;
    std::string_view build_var;
};

inline constexpr std::array<CheckboxBinding, 11> checkbox_bindings{{
    {&Ui::ConfOptionsPage::hardly_check, &ConfigOptions::hardly_check, "hardly"},
    {&Ui::ConfOptionsPage::perfgovern_check, &ConfigOptions::per_gov_check, "per_gov"},
    {&Ui::ConfOptionsPage::tcpbbr_check, &ConfigOptions::tcp_bbr3_check, "tcp_bbr3"},
    {&Ui::ConfOptionsPage::customconfig_check, &ConfigOptions::custom_config_check, "custom_config"},
    {&Ui::ConfOptionsPage::nconfig_check, &ConfigOptions::nconfig_check, "nconfig"},
    {&Ui::ConfOptionsPage::xconfig_check, &ConfigOptions::xconfig_check, "xconfig"},
    {&Ui::ConfOptionsPage::localmodcfg_check, &ConfigOptions::localmodcfg_check, "localmodcfg"},
    {&Ui::ConfOptionsPage::use_current_check, &ConfigOptions::use_current_check, "use_current"},
    {&Ui::ConfOptionsPage::builtin_zfs_check, &ConfigOptions::builtin_zfs_check, "builtin_zfs"},
    {&Ui::ConfOptionsPage::builtin_nvidia_open_check, &ConfigOptions::builtin_nvidia_open_check, "builtin_nvidia_open"},
    {&Ui::ConfOptionsPage::build_debug_check, &ConfigOptions::build_debug_check, "build_debug"},
}};

// Option rows of the Configure page mapped to their per-repo option key, so
// the visible option set can follow the selected build source.
inline constexpr std::array<std::pair<std::string_view, QWidget* Ui::ConfOptionsPage::*>, 17> option_row_bindings{{
    {"hardly", &Ui::ConfOptionsPage::hardly_widget},
    {"per_gov", &Ui::ConfOptionsPage::perfgovern_widget},
    {"tcp_bbr3", &Ui::ConfOptionsPage::tcpbbr_widget},
    {"custom_config", &Ui::ConfOptionsPage::customconfig_widget},
    {"nconfig", &Ui::ConfOptionsPage::nconfig_widget},
    {"xconfig", &Ui::ConfOptionsPage::xconfig_widget},
    {"localmodcfg", &Ui::ConfOptionsPage::localmodcfg_widget},
    {"use_current", &Ui::ConfOptionsPage::use_current_widget},
    {"builtin_zfs", &Ui::ConfOptionsPage::builtin_zfs_widget},
    {"builtin_nvidia_open", &Ui::ConfOptionsPage::builtin_nvidia_open_widget},
    {"build_debug", &Ui::ConfOptionsPage::build_debug_widget},
    {"HZ_ticks", &Ui::ConfOptionsPage::hzticks_widget},
    {"tickrate", &Ui::ConfOptionsPage::tickless_widget},
    {"preempt", &Ui::ConfOptionsPage::preempt_widget},
    {"hugepage", &Ui::ConfOptionsPage::hugepage_widget},
    {"cpu_opt", &Ui::ConfOptionsPage::processor_opt_widget},
    {"lto", &Ui::ConfOptionsPage::lto_widget},
}};

inline auto set_combobox_val(QComboBox* combobox, ssize_t index) noexcept {
    if (index < 0) {
        return 1;
    }
    combobox->setCurrentIndex(static_cast<std::int32_t>(index));
    return 0;
}

// The last path component of a build source (scheme and ".git" stripped).
auto repo_basename(std::string_view source) noexcept -> std::string {
    auto name = source;
    if (const auto scheme_pos = name.find("://"); scheme_pos != std::string_view::npos) {
        name = name.substr(scheme_pos + 3);
    }
    if (const auto slash = name.find_last_of('/'); slash != std::string_view::npos) {
        name = name.substr(slash + 1);
    }
    if (name.ends_with(".git")) {
        name.remove_suffix(4);
    }
    return std::string{name};
}

// Normalize a user-typed build source (AUR package name or git URL) to the
// per-repo option-map key (repo basename with '-' replaced by '_').
auto normalize_repo_key(std::string_view source) noexcept -> std::string {
    auto key = repo_basename(source);
    std::ranges::replace(key.begin(), key.end(), '-', '_');
    return key;
}

// Per-repo option map selection: the resolver and the maps themselves are
// generated from src/compile_options.json (detail::resolve_option_map); a
// source without a dedicated map yields nullopt (no dedicated options).
using option_map_ref = std::optional<detail::option_maps_variant>;

// Look up the build variable an option maps to in the given per-repo map
// (nullopt when the repo does not offer the option).
auto option_build_var(option_map_ref map, std::string_view option) noexcept -> std::optional<std::string_view> {
    if (!map) {
        return std::nullopt;
    }
    return std::visit(
        [&](const auto* m) noexcept -> std::optional<std::string_view> {
            if (const auto it = m->find(frozen::string{option}); it != m->end()) {
                return std::string_view{it->second};
            }
            return std::nullopt;
        },
        *map);
}

// True when the repo carries a values table for the option, i.e. at least
// one "<repo>|<option>|*" entry in the generated translation map. For such
// options the table is the closed set of buildable UI values.
auto has_value_xform_table(std::string_view repo, std::string_view option) noexcept -> bool {
    const std::string prefix{std::string{repo} + "|" + std::string{option} + "|"};
    for (const auto& entry : detail::value_xforms) {
        if (std::string_view{entry.first.data(), entry.first.size()}.starts_with(prefix)) {
            return true;
        }
    }
    return false;
}

// Translate a UI option value to the value the active repo's build machinery
// expects (generated detail::value_xforms, "<repo>|<option>|<ui_value>" ->
// dst): nullopt (the caller emits nothing, the cpu_opt "manual" contract)
// when the value has no build value — an empty translation, or a value
// absent from the option's closed values table; otherwise the translation,
// or the value itself when the option carries no table (plain pass-through).
auto xformed_value(std::string_view repo, std::string_view option, std::string_view value) noexcept -> std::optional<std::string> {
    const std::string key{std::string{repo} + "|" + std::string{option} + "|" + std::string{value}};
    if (const auto it = detail::value_xforms.find(frozen::string{key}); it != detail::value_xforms.end()) {
        if (it->second.empty()) {
            fmt::print(stderr, "option {}={} has no build value for repo {}; not emitted\n", option, value, repo);
            return std::nullopt;
        }
        return std::string{it->second};
    }
    if (has_value_xform_table(repo, option)) {
        fmt::print(stderr, "option {}={} is not buildable for repo {}; not emitted\n", option, value, repo);
        return std::nullopt;
    }
    return std::string{value};
}

inline auto convert_to_var_assign(option_map_ref map, std::string_view repo, std::string_view option, std::string_view value) noexcept -> std::string {
    const auto var = option_build_var(map, option);
    if (!var) {
        return std::string{};  // the active repo does not offer this option
    }
    if (const auto xformed = xformed_value(repo, option, value)) {
        return fmt::format(FMT_COMPILE("{}={}\n"), *var, *xformed);
    }
    return std::string{};  // the value has no build value for this repo (not emitted, like "manual")
}

/// return flag to enable if the option is enabled, otherwise do nothing
inline auto convert_to_var_assign_empty_wrapped(option_map_ref map, std::string_view repo, std::string_view option_name, bool option_enabled) noexcept -> std::string {
    using namespace std::string_view_literals;
    if (option_enabled) {
        return convert_to_var_assign(map, repo, option_name, "yes"sv);
    }
    return convert_to_var_assign(map, repo, option_name, "no"sv);
}

// Creates a transient executable testscript, runs it, and removes the file
// on every return path (success, command failure, and popen error alike):
// the script only sources the PKGBUILD / makepkg.conf so it can be parsed,
// and must never linger in the source tree or the user's working directory.
auto run_and_remove_testscript(std::string_view script_path, std::string_view script_src, std::string_view args = {}) noexcept -> std::string {
    if (!utils::write_to_file(script_path, script_src)) {
        // Never exec a script that could not be written: popen would just
        // fail and pollute the result with "-1" noise.
        fmt::print(stderr, "run_and_remove_testscript: failed to write '{}'; not executing\n", std::string{script_path});
        return {};
    }
    fs::permissions(script_path,
        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
        fs::perm_options::add);

    const std::string command = args.empty() ? std::string{script_path} : fmt::format(FMT_COMPILE("{} {}"), std::string{script_path}, std::string{args});
    const std::string result  = utils::exec(command);

    std::error_code ec{};
    fs::remove(script_path, ec);
    if (ec) {
        fmt::print(stderr, "failed to remove testscript '{}': {}\n", std::string{script_path}, ec.message());
    }
    return result;
}

auto get_source_array_from_pkgbuild(std::string_view kernel_name_path, std::string_view options_set) noexcept {
    const auto& testscript_src  = fmt::format(FMT_COMPILE("#!/usr/bin/bash\n{}\nsource \"$1\"\n{}"), options_set, "echo \"${source[@]}\"");
    const auto& testscript_path = fmt::format(FMT_COMPILE("{}/.testscript"), kernel_name_path);

    const auto& src_entries = run_and_remove_testscript(testscript_path, testscript_src, fmt::format(FMT_COMPILE("{}/PKGBUILD"), kernel_name_path));
    return utils::make_multiline(src_entries, ' ');
}

auto get_pkgext_value_from_makepkgconf() noexcept -> std::string {
    using namespace std::string_view_literals;
    using namespace std::string_literals;
    static constexpr auto testscript_src = "#!/usr/bin/bash\nsource \"/etc/makepkg.conf\"\necho \"${PKGEXT}\""sv;

    // Pinned to the absolute build repo root (not the process CWD): the
    // transient testscript must land in a known, existing directory.
    const auto& testscript_path = fmt::format(FMT_COMPILE("{}/.testscriptpkgext"), utils::build_repo_path().string());

    auto pkgext_val = run_and_remove_testscript(testscript_path, testscript_src);
    if (pkgext_val.empty()) {
        fmt::print(stderr, "failed to get PKGEXT from /etc/makepkg.conf");
        return ".pkg.tar.zst"s;
    }
    return pkgext_val;
}

auto prepare_func_names(std::vector<std::string> parse_lines, std::string_view pkgver_str) noexcept -> std::vector<std::string> {
    using namespace std::string_view_literals;

    static constexpr auto functor = [](auto&& rng) {
        auto rng_str = std::string_view(&*rng.begin(), static_cast<size_t>(std::ranges::distance(rng)));
        return rng_str.starts_with("package_"sv);
    };

    // fetch the pkgext from /etc/makepkg.conf, and fallback to '.pkg.tar.zst' which is default value of makepkg
    const auto& pkgext_val = get_pkgext_value_from_makepkgconf();

    std::vector<std::string> pkg_globs{};
    pkg_globs = parse_lines
        | std::ranges::views::transform([&](auto&& rng) {
              auto&& line = std::string_view(&*rng.begin(), static_cast<size_t>(std::ranges::distance(rng)));

              static constexpr auto needle_prefix = "declare -f "sv;
              if (line.starts_with(needle_prefix)) {
                  line.remove_prefix(needle_prefix.size());
              }
              return line;
          })
        | std::ranges::views::filter(functor)
        | std::ranges::views::transform([&](auto&& rng) {
              auto&& line = std::string_view(&*rng.begin(), static_cast<size_t>(std::ranges::distance(rng)));

              static constexpr auto needle_prefix = "package_"sv;
              if (line.starts_with(needle_prefix)) {
                  line.remove_prefix(needle_prefix.size());
              }
              return fmt::format(FMT_COMPILE("{}-{}-*{}"), line, pkgver_str, pkgext_val);
          })
        | std::ranges::to<std::vector<std::string>>();
    return pkg_globs;
}

auto get_package_names_glob_from_pkgbuild(std::string_view kernel_name_path) noexcept -> std::vector<std::string> {
    using namespace std::string_view_literals;
    static constexpr auto testscript_src = "#!/usr/bin/bash\nsource \"$1\"\ndeclare -F;echo \"pkgver: $pkgver-$pkgrel\""sv;
    static constexpr auto pkgver_prefix  = "pkgver: "sv;

    const auto& testscript_path = fmt::format(FMT_COMPILE("{}/.testscriptpkgnames"), kernel_name_path);

    const auto& src_entries = run_and_remove_testscript(testscript_path, testscript_src, fmt::format(FMT_COMPILE("{}/PKGBUILD"), kernel_name_path));
    const auto& parse_lines = utils::make_multiline(src_entries, '\n');

    auto it = std::ranges::find_if(parse_lines, [](auto&& line) { return line.starts_with(pkgver_prefix); });
    if (it == std::ranges::end(parse_lines)) {
        fmt::print(stderr, "broken pkgbuild; pkgver must be present\n");
        return {};
    }
    auto pkgver_str = std::string_view{*it};
    pkgver_str.remove_prefix(pkgver_prefix.size());

    return prepare_func_names(parse_lines, pkgver_str);
}

bool insert_new_source_array_into_pkgbuild(std::string_view kernel_name_path, QListWidget* list_widget, const std::vector<std::string>& orig_source_array) noexcept {
    static constexpr auto functor = [](auto&& rng) {
        auto rng_str = std::string_view(&*rng.begin(), static_cast<size_t>(std::ranges::distance(rng)));
        return !rng_str.ends_with(".patch");
    };

    auto array_entries = orig_source_array
        | std::ranges::views::filter(functor)
        | std::ranges::views::transform([](auto&& rng) { return fmt::format(FMT_COMPILE("\"{}\""), rng); })
        | std::ranges::to<std::vector<std::string>>();

    // Apply flag to each item in list widget
    for (int i = 0; i < list_widget->count(); ++i) {
        auto* item = list_widget->item(i);
        array_entries.emplace_back(fmt::format(FMT_COMPILE("\"{}\""), item->text().toStdString()));
    }
    const auto& pkgbuild_path = fmt::format(FMT_COMPILE("{}/PKGBUILD"), kernel_name_path);
    auto pkgbuildsrc          = utils::read_whole_file(pkgbuild_path);
    if (pkgbuildsrc.empty()) {
        // Never rewrite on empty content: the PKGBUILD is missing or
        // unreadable, so the source array cannot be (re)placed safely.
        fmt::print(stderr, "insert_new_source_array: PKGBUILD at '{}' is missing or unreadable; file left untouched\n", pkgbuild_path);
        return false;
    }

    const auto& new_source_array = fmt::format(FMT_COMPILE("source=(\n{})\n"), array_entries | std::ranges::views::join_with('\n') | std::ranges::to<std::string>());
    if (auto foundpos = pkgbuildsrc.find("prepare()"); foundpos != std::string::npos) {
        if (auto last_newline_before = pkgbuildsrc.find_last_of('\n', foundpos); last_newline_before != std::string::npos) {
            pkgbuildsrc.insert(last_newline_before, new_source_array);
        }
    }
    return utils::write_to_file(pkgbuild_path, pkgbuildsrc);
}

bool set_custom_name_in_pkgbuild(std::string_view kernel_name_path, std::string_view custom_name) noexcept {
    const auto& pkgbuild_path = fmt::format(FMT_COMPILE("{}/PKGBUILD"), kernel_name_path);
    auto pkgbuildsrc          = utils::read_whole_file(pkgbuild_path);
    if (pkgbuildsrc.empty()) {
        fmt::print(stderr, "set_custom_name: cannot read '{}'\n", pkgbuild_path);
        return false;
    }

    // Generic pkgbase=/pkgver= detection: a foreign PKGBUILD lacking the
    // expected structure fails cleanly and the file is left untouched.
    const auto result = utils::apply_pkgbuild_custom_name(std::move(pkgbuildsrc), custom_name);
    if (!result.ok) {
        fmt::print(stderr, "set_custom_name: {}\n", result.error);
        return false;
    }
    return utils::write_to_file(pkgbuild_path, result.new_content);
}

auto convert_vector_of_strings_to_stringlist(const std::vector<std::string>& vec) noexcept {
    QStringList result{};

    for (auto&& element : vec) {
        result << QString::fromStdString(element);
    }
    return result;
}

inline void list_widget_apply_edit_flag(QListWidget* list_widget) noexcept {
    // Apply flag to each item in list widget
    for (int i = 0; i < list_widget->count(); ++i) {
        auto* item = list_widget->item(i);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
    }
}

}  // namespace

// NOTE: we use std::string const ref intentionally to prevent conversion from string_view into QString
void ConfWindow::run_cmd_async(std::string cmd, const std::string& working_path, bool escalate, bool expect_done) noexcept {
    // Re-arm the completion context for every launch: a re-launch while the
    // done-status poll is running (the post-build pacman -U) must not leave
    // a stale timer aimed at the old build dir.
    m_done_status_timer.stop();
    m_expect_done = expect_done;
    m_last_helper_rc = 0;

    using namespace std::string_literals;
    cmd += "; read -p 'Press enter to exit'"s;

    // remember current build working directory
    m_build_conf_path = working_path;

    m_cmd.setProgram(QStringLiteral(KM_HELPER_DIR "/terminal-helper"));
    QStringList arguments{};
    if (escalate) {
        // Unified polkit privilege layer (D4): same pkexec rootshell.sh path as kernel install/remove
        arguments << "-s" << QString("pkexec " KM_HELPER_DIR "/rootshell.sh");
    }
    arguments << QString::fromStdString(cmd);
    m_cmd.setArguments(arguments);
    m_cmd.setWorkingDirectory(QString::fromStdString(working_path));

    m_cmd.start();

    // connect finish callback
    connect(&m_cmd, &QProcess::finished, this, &ConfWindow::finished_proc, Qt::UniqueConnection);
}

void ConfWindow::finished_proc(int exit_code, QProcess::ExitStatus) noexcept {
    m_running = false;
    m_last_helper_rc = exit_code;

    // The terminal-helper's lifetime equals the launched command's lifetime
    // (the D2 contract), so this event fires when the command ends. The
    // .done-status marker (touched by the build command) is the completion
    // signal for expect_done launches; marker-less launches (the post-build
    // pacman -U, escalated ops) end normally without one.
    const auto& check_tmp_path = fmt::format(FMT_COMPILE("{}/.done-status"), m_build_conf_path);
    if (fs::exists(check_tmp_path) && m_expect_done) {
        // The success path: the build command ran to completion.
        handle_build_done();
    } else if (!m_expect_done) {
        // A marker-less launch ending is normal, not a failure — the old
        // code printed a spurious "process failed with exit code: N" here.
    } else {
        // Expect a marker but none yet: the transient race where the marker
        // lands slightly after the helper's liveness loop ended. Bounded
        // belt-and-braces poll (10 x 3 s) before declaring failure.
        m_done_polls_left = 10;
        fmt::print(stderr, "build completion marker not found yet; polling (bounded 30 s)\n");
        m_done_status_timer.start(3000);
    }
}

// The build-completion success path (extracted from finished_proc, cycle-7
// C3/D3): the .done-status marker was found, so remove it and offer the
// post-build package install. Reached from finished_proc (the marker hit at
// command end) and from on_done_status_tick (the bounded poll).
void ConfWindow::handle_build_done() noexcept {
    const auto& check_tmp_path = fmt::format(FMT_COMPILE("{}/.done-status"), m_build_conf_path);
    fs::remove(check_tmp_path);

    fmt::print("success\n");

    auto res = QMessageBox::question(this, tr("Kernel Manager"), tr("Do you want to install build packages?"));
    if (res == QMessageBox::Yes) {
        fmt::print("pressed yes\n");

        auto pkg_glob_list = get_package_names_glob_from_pkgbuild(m_build_conf_path);
        // Absolute paths: the pkexec'd root shell (escalate=true) starts in $HOME, so a
        // bare glob would expand against the wrong directory and match nothing.
        auto pkg_globs = pkg_glob_list
                         | std::ranges::views::transform(
                               [d = m_build_conf_path](const auto& g) { return d + "/" + g; })
                         | std::ranges::views::join_with(' ')
                         | std::ranges::to<std::string>();
        auto pacman_cmd = fmt::format(FMT_COMPILE("pacman -U {}"), pkg_globs);

        fmt::print("pacman_cmd := {}\n", pacman_cmd);
        m_running = true;
        run_cmd_async(pacman_cmd, m_build_conf_path, /*escalate=*/true, /*expect_done=*/false);
    }
}

// The bounded done-status poll (cycle-7 C3/D3): 10 x 3 s retries of the
// marker check after a marker-less helper exit. Marker found => the success
// path; polls exhausted => an honest failure diagnostic (the old code
// printed a misleading "process failed with exit code: 2" the moment the
// helper exited).
void ConfWindow::on_done_status_tick() noexcept {
    const auto& check_tmp_path = fmt::format(FMT_COMPILE("{}/.done-status"), m_build_conf_path);
    if (fs::exists(check_tmp_path)) {
        m_done_status_timer.stop();
        handle_build_done();
        return;
    }
    if (m_done_polls_left > 1) {
        --m_done_polls_left;
        m_done_status_timer.start(3000);
        return;
    }
    m_done_status_timer.stop();
    fmt::print(stderr, "build failed: no completion marker after the wait window (terminal exit code: {})\n", m_last_helper_rc);
}

void ConfWindow::connect_all_checkboxes() noexcept {
    auto* options_page_ui_obj = m_ui->conf_options_page_widget->get_ui_obj();

    const std::array checkbox_list{
        options_page_ui_obj->builtin_nvidia_open_check,
    };

    for (auto* checkbox : checkbox_list) {
        connect(checkbox, &QCheckBox::checkStateChanged, this, [this](Qt::CheckState) {
            reset_patches_data_tab();
        });
    }
}

std::string ConfWindow::get_all_set_values() const noexcept {
    std::string result{};
    auto* options_page_ui_obj = m_ui->conf_options_page_widget->get_ui_obj();

    // The option set is per-repo: resolved from the selected build source
    // (single utils accessor, synced from the UI before builds) by the
    // generated resolver. Options the active repo does not offer are skipped
    // entirely; emitted values pass through the generated value translation
    // (xformed_value), which may also withhold an unbuildable value.
    const std::string repo_key = normalize_repo_key(utils::build_source_repo());
    const auto map = detail::resolve_option_map(repo_key);

    // checkboxes values,
    // which becomes enabled with any value passed,
    // and if nothing passed means it's disabled.
    for (const auto& binding : checkbox_bindings) {
        result += convert_to_var_assign_empty_wrapped(map, repo_key, binding.build_var, checkstate_checked(options_page_ui_obj->*binding.widget));
    }

    // combobox values
    result += convert_to_var_assign(map, repo_key, "HZ_ticks", get_hz_tick(static_cast<size_t>(options_page_ui_obj->hzticks_combo_box->currentIndex())));
    result += convert_to_var_assign(map, repo_key, "tickrate", get_tickless_mode(static_cast<size_t>(options_page_ui_obj->tickless_combo_box->currentIndex())));
    result += convert_to_var_assign(map, repo_key, "preempt", get_preempt_mode(static_cast<size_t>(options_page_ui_obj->preempt_combo_box->currentIndex())));
    result += convert_to_var_assign(map, repo_key, "hugepage", get_hugepage_mode(static_cast<size_t>(options_page_ui_obj->hugepage_combo_box->currentIndex())));
    result += convert_to_var_assign(map, repo_key, "lto", get_lto_mode(static_cast<size_t>(options_page_ui_obj->lto_combo_box->currentIndex())));

    const std::string_view cpu_opt_mode = get_cpu_opt_mode(static_cast<size_t>(options_page_ui_obj->processor_opt_combo_box->currentIndex()));
    if (cpu_opt_mode != "manual") {
        result += convert_to_var_assign(map, repo_key, "cpu_opt", cpu_opt_mode);
    }

    // NOTE: workaround PKGBUILD incorrectly working with custom pkgname
    // (only for repos whose map offers LTO)
    if (option_build_var(map, "lto").has_value()) {
        const std::string_view lto_mode = get_lto_mode(static_cast<size_t>(options_page_ui_obj->lto_combo_box->currentIndex()));
        if (lto_mode != "none" && options_page_ui_obj->custom_name_edit->text() != "$pkgbase") {
            result += "_use_lto_suffix=n\n";
        }
    }

    return result;
}

void ConfWindow::clear_patches_data_tab() noexcept {
    auto* patches_page_ui_obj = m_ui->conf_patches_page_widget->get_ui_obj();
    patches_page_ui_obj->list_widget->clear();
}

auto ConfWindow::kernel_path_for_index(std::int32_t index) const noexcept -> std::string {
    if (index < 0 || index >= static_cast<std::int32_t>(m_flavors.size())) {
        return {};
    }
    return m_flavors[static_cast<std::size_t>(index)].path;
}

// (Re)discover the build flavors from the kernel source repo and repopulate
// the flavor combo, preserving the user's current selection when possible.
void ConfWindow::refresh_flavors() noexcept {
    auto* options_page_ui_obj = m_ui->conf_options_page_widget->get_ui_obj();
    auto* combo               = options_page_ui_obj->main_combo_box;

    // remember the currently selected flavor key (if any) to restore it
    std::string previous_key{};
    if (const std::int32_t cur = combo->currentIndex(); cur >= 0 && cur < static_cast<std::int32_t>(m_flavors.size())) {
        previous_key = m_flavors[static_cast<std::size_t>(cur)].key;
    }

    m_flavors = discover_repo_flavors(utils::build_repo_path(), repo_basename(utils::build_source_repo()));
    if (m_flavors.empty()) {
        return;
    }

    const std::string base_key = m_flavors.front().key;

    {
        const QSignalBlocker blocker(combo);
        combo->clear();
        for (const auto& flavor : m_flavors) {
            combo->addItem(QString::fromStdString(flavor_display_name(flavor.key, base_key)), QString::fromStdString(flavor.key));
        }
    }

    // restore the previous selection, falling back to the base flavor
    std::int32_t target_index = 0;
    for (std::int32_t i = 0; i < static_cast<std::int32_t>(m_flavors.size()); ++i) {
        if (m_flavors[static_cast<std::size_t>(i)].key == previous_key) {
            target_index = i;
            break;
        }
    }
    combo->setCurrentIndex(target_index);
}

// Sync the build source selected in the UI into the single utils accessor
// used by prepare_build_environment(): the effective source is the custom
// URL text when the "Custom URL…" entry is selected, otherwise the selected
// known source.
void ConfWindow::sync_build_source() noexcept {
    utils::set_build_source_repo(effective_build_source());
}

// Make the option rows reflect the option set of the repo selected in the
// source field (unknown sources fall back to the generic reduced set).
void ConfWindow::update_option_set() noexcept {
    auto* options_page_ui_obj = m_ui->conf_options_page_widget->get_ui_obj();
    const auto map = detail::resolve_option_map(normalize_repo_key(effective_build_source()));
    for (const auto& [option_key, row] : option_row_bindings) {
        (options_page_ui_obj->*row)->setVisible(option_build_var(map, option_key).has_value());
    }
}

// The effective build source: the custom URL text when the "Custom URL…"
// entry is selected, otherwise the selected known source.
auto ConfWindow::effective_build_source() const noexcept -> std::string {
    auto* options_page_ui_obj = m_ui->conf_options_page_widget->get_ui_obj();
    auto* source_combo        = options_page_ui_obj->build_source_combo;
    const std::int32_t index  = source_combo->currentIndex();
    if (index < 0) {
        return {};
    }
    if (index == source_combo->count() - 1) {  // the trailing "Custom URL…" entry
        return options_page_ui_obj->build_source_custom_edit->text().toStdString();
    }
    return source_combo->itemText(index).toStdString();
}

// Set the source dropdown to a given build source: a known item selects its
// combo entry, anything else falls back to the "Custom URL…" entry with the
// line edit prefilled.
void ConfWindow::set_build_source(const std::string& source) noexcept {
    if (source.empty()) {
        return;
    }
    auto* options_page_ui_obj = m_ui->conf_options_page_widget->get_ui_obj();
    auto* source_combo        = options_page_ui_obj->build_source_combo;

    {
        // Apply the selection without firing the change handlers (which
        // update_build_source_ui below runs exactly once, unblocked).
        const QSignalBlocker combo_blocker(source_combo);
        const QSignalBlocker custom_blocker(options_page_ui_obj->build_source_custom_edit);
        if (const std::int32_t index = source_combo->findText(QString::fromStdString(source)); index >= 0) {
            source_combo->setCurrentIndex(index);
        } else {
            source_combo->setCurrentIndex(source_combo->count() - 1);  // Custom URL…
            options_page_ui_obj->build_source_custom_edit->setText(QString::fromStdString(source));
        }
    }
    update_build_source_ui();
}

// Show or hide the custom URL edit according to the dropdown selection and
// keep the visible option rows in sync with the effective source.
void ConfWindow::update_build_source_ui() noexcept {
    auto* options_page_ui_obj = m_ui->conf_options_page_widget->get_ui_obj();
    auto* source_combo        = options_page_ui_obj->build_source_combo;
    const bool is_custom      = (source_combo->currentIndex() == source_combo->count() - 1);

    options_page_ui_obj->build_source_custom_edit->setVisible(is_custom);
    if (is_custom) {
        options_page_ui_obj->build_source_custom_edit->setFocus();
    }
    update_option_set();
}

// Auto-populate the source dropdown from a kernel selected in the main
// window tree: strip the repo prefix and map the name to its default source
// from the known-kernels table (a source that is not a known one falls back
// to the "Custom URL…" entry prefilled). A repeat for the same kernel is a
// no-op, so this never fights a manual source choice.
void ConfWindow::apply_source_for_kernel(std::string_view kernel_name) noexcept {
    const std::string_view name = km::kernel_name_from_raw(kernel_name);
    if (name.empty() || name == m_last_applied_kernel) {
        return;
    }
    m_last_applied_kernel = std::string{name};
    set_build_source(km::default_source_for(name));
}

void ConfWindow::reset_patches_data_tab() noexcept {
    auto* options_page_ui_obj = m_ui->conf_options_page_widget->get_ui_obj();
    auto* patches_page_ui_obj = m_ui->conf_patches_page_widget->get_ui_obj();

    const std::int32_t main_combo_index = options_page_ui_obj->main_combo_box->currentIndex();
    const auto cpusched_path            = kernel_path_for_index(main_combo_index);
    if (cpusched_path.empty()) {
        fmt::print(stderr, "no kernel flavor available yet (source repo not prepared?); use Configure first\n");
        return;
    }

    auto current_array_items = get_source_array_from_pkgbuild(cpusched_path, get_all_set_values());
    std::erase_if(current_array_items, [](auto&& item_el) { return !item_el.ends_with(".patch"); });

    clear_patches_data_tab();
    patches_page_ui_obj->list_widget->addItems(convert_vector_of_strings_to_stringlist(current_array_items));

    // Apply flag to each item in list widget
    list_widget_apply_edit_flag(patches_page_ui_obj->list_widget);
}

ConfWindow::ConfWindow(QWidget* parent)
  : QMainWindow(parent) {
    m_ui->setupUi(this);
    setWindowIcon(QApplication::windowIcon());  // explicit dedicated icon; the .ui no longer overrides; robust to Qt app-fallback semantics

    setAttribute(Qt::WA_NativeWindow);
    setWindowFlags(Qt::Window);  // for the close, min and max buttons

    auto* options_page_ui_obj = m_ui->conf_options_page_widget->get_ui_obj();
    auto* patches_page_ui_obj = m_ui->conf_patches_page_widget->get_ui_obj();

    // Selecting the kernel flavor: the list is discovered from the kernel
    // source repo (populated/refreshed via refresh_flavors), so no flavor
    // names are hard-coded here.
    refresh_flavors();

    // Build source dropdown: the known package sources (from the
    // known-kernels table) plus a "Custom URL…" entry that reveals a line
    // edit for an arbitrary AUR package name or git URL. Initialized from
    // the single utils accessor; the visible option rows follow the
    // effective source.
    {
        auto* source_combo = options_page_ui_obj->build_source_combo;
        const QSignalBlocker combo_blocker(source_combo);
        for (const auto& source : km::known_sources()) {
            source_combo->addItem(QString::fromStdString(source));
        }
        source_combo->addItem(tr("Custom URL…"));
    }
    connect(options_page_ui_obj->build_source_combo, &QComboBox::currentIndexChanged, this, [this](std::int32_t) {
        update_build_source_ui();
    });
    connect(options_page_ui_obj->build_source_custom_edit, &QLineEdit::textChanged, this, [this](const QString&) {
        update_option_set();
    });
    set_build_source(utils::build_source_repo());

    // Setting default options
    options_page_ui_obj->customconfig_check->setCheckState(Qt::Checked);
    options_page_ui_obj->hardly_check->setCheckState(Qt::Checked);

    QStringList hz_ticks;
    hz_ticks << "1000HZ"
             << "750Hz"
             << "600Hz"
             << "500Hz"
             << "300Hz"
             << "250Hz"
             << "100Hz";
    options_page_ui_obj->hzticks_combo_box->addItems(hz_ticks);

    QStringList tickless_modes;
    tickless_modes << "Full"
                   << "Idle"
                   << "Periodic";
    options_page_ui_obj->tickless_combo_box->addItems(tickless_modes);

    QStringList preempt_modes;
    preempt_modes << "Full"
                  << "Lazy";
    options_page_ui_obj->preempt_combo_box->addItems(preempt_modes);

    /* clang-format off */
    QStringList cpu_optims;
    cpu_optims << "Disabled"
               << "Native CPU"
               << "Generic / x86_64"
               << "x86_64_v2" << "x86_64_v3" << "x86_64_v4"
               << "Zen4";
    options_page_ui_obj->processor_opt_combo_box->addItems(cpu_optims);
    /* clang-format on */

    QStringList lto_modes;
    lto_modes << "No"
              << "Full"
              << "Thin"
              << "Thin-dist";
    options_page_ui_obj->lto_combo_box->addItems(lto_modes);
    // Default (initial selection) is Thin
    options_page_ui_obj->lto_combo_box->setCurrentIndex(static_cast<int>(lookup_lto_mode("thin")));

    QStringList hugepage_modes;
    hugepage_modes << "Always"
                   << "Madvise";
    options_page_ui_obj->hugepage_combo_box->addItems(hugepage_modes);

    // Connect buttons signal
    connect(options_page_ui_obj->cancel_button, &QPushButton::clicked, this, &ConfWindow::on_cancel);
    connect(options_page_ui_obj->ok_button, &QPushButton::clicked, this, &ConfWindow::on_execute);
    connect(options_page_ui_obj->save_button, &QPushButton::clicked, this, &ConfWindow::on_save);
    connect(options_page_ui_obj->load_button, &QPushButton::clicked, this, &ConfWindow::on_load);
    connect(options_page_ui_obj->main_combo_box, &QComboBox::currentIndexChanged, this, [this, options_page_ui_obj](std::int32_t main_combo_index) {
        if (main_combo_index < 0 || main_combo_index >= static_cast<std::int32_t>(m_flavors.size())) {
            return;
        }
        using namespace std::string_view_literals;
        const std::string_view kernel_name = m_flavors[static_cast<std::size_t>(main_combo_index)].key;
        const std::string_view base_key    = m_flavors.front().key;

        // Block signals to prevent cascading combo box updates
        const QSignalBlocker preempt_blocker(options_page_ui_obj->preempt_combo_box);
        const QSignalBlocker lto_blocker(options_page_ui_obj->lto_combo_box);
        const QSignalBlocker hz_blocker(options_page_ui_obj->hzticks_combo_box);
        const QSignalBlocker customconfig_blocker(options_page_ui_obj->customconfig_check);
        const QSignalBlocker zfs_blocker(options_page_ui_obj->builtin_zfs_check);

        // thin-dist is not available for lts and hardened
        const bool has_thin_dist = (kernel_name != "lts"sv && kernel_name != "hardened"sv);
        if (has_thin_dist && options_page_ui_obj->lto_combo_box->count() == 3) {
            options_page_ui_obj->lto_combo_box->addItem(QStringLiteral("Thin-dist"));
        } else if (!has_thin_dist && options_page_ui_obj->lto_combo_box->count() == 4) {
            options_page_ui_obj->lto_combo_box->removeItem(options_page_ui_obj->lto_combo_box->count() - 1);
        }

        // thin for base/rc flavors, none for others
        const bool lto_thin_default = (kernel_name == base_key || kernel_name == "rc"sv);
        options_page_ui_obj->lto_combo_box->setCurrentIndex(static_cast<int>(lto_thin_default ? lookup_lto_mode("thin") : lookup_lto_mode("none")));

        // voluntary/none only available for hardened and lts
        const bool has_extended_preempt = (kernel_name == "hardened"sv || kernel_name == "lts"sv);
        if (has_extended_preempt && options_page_ui_obj->preempt_combo_box->count() == 2) {
            options_page_ui_obj->preempt_combo_box->addItem(QStringLiteral("Voluntary"));
            options_page_ui_obj->preempt_combo_box->addItem(QStringLiteral("None"));
        } else if (!has_extended_preempt && options_page_ui_obj->preempt_combo_box->count() == 4) {
            options_page_ui_obj->preempt_combo_box->removeItem(options_page_ui_obj->preempt_combo_box->count() - 1);
            options_page_ui_obj->preempt_combo_box->removeItem(options_page_ui_obj->preempt_combo_box->count() - 1);
        }

        // lazy for server, full for others
        options_page_ui_obj->preempt_combo_box->setCurrentIndex(static_cast<int>((kernel_name == "server"sv) ? lookup_preempt_mode("lazy") : lookup_preempt_mode("full")));

        // 300 for server, 1000 for others
        options_page_ui_obj->hzticks_combo_box->setCurrentIndex(static_cast<int>((kernel_name == "server"sv) ? lookup_hz_tick("300") : lookup_hz_tick("1000")));

        // unchecked for server, checked for others
        set_checkstate(options_page_ui_obj->customconfig_check, kernel_name != "server"sv);

        // incompatible with realtime kernels
        const bool is_realtime = kernel_name.starts_with("rt"sv);
        options_page_ui_obj->builtin_zfs_check->setEnabled(!is_realtime);
        if (is_realtime) {
            set_checkstate(options_page_ui_obj->builtin_zfs_check, false);
        }

        reset_patches_data_tab();
    });
    connect(options_page_ui_obj->lto_combo_box, &QComboBox::currentIndexChanged, this, [this](std::int32_t) {
        reset_patches_data_tab();
    });

    // Setup patches page
    // TODO(vnepogodin): make it lazy loading, only if the user launched the configure window.
    // on window opening setup the page(clone git repo & reset values) run in the background -> show progress bar.
    // prepare_build_environment();
    // reset_patches_data_tab();
    connect_all_checkboxes();

    // local patches
    connect(patches_page_ui_obj->local_patch_button, &QPushButton::clicked, this, [this, patches_page_ui_obj] {
        auto files = QFileDialog::getOpenFileNames(
            this,
            tr("Select one or more patch files"),
            QString::fromStdString(utils::fix_path("~/")),
            tr("Patch file (*.patch)"));
        /* clang-format off */
        if (files.isEmpty()) { return; }
        /* clang-format on */

        // Prepend 'file://' to each selected patch file.
        std::ranges::transform(files,
            files.begin(),  // write to the same location
            [](auto&& file_path) { return QString("file://") + std::forward<decltype(file_path)>(file_path); });

        patches_page_ui_obj->list_widget->addItems(files);

        // Apply flag to each item in list widget
        list_widget_apply_edit_flag(patches_page_ui_obj->list_widget);
    });
    // remote patches
    connect(patches_page_ui_obj->remote_patch_button, &QPushButton::clicked, this, [this, patches_page_ui_obj] {
        bool is_confirmed{};
        const auto& patch_url_text = QInputDialog::getText(
            this,
            tr("Enter URL patch"),
            tr("Patch URL:"), QLineEdit::Normal,
            QString(), &is_confirmed);
        /* clang-format off */
        if (!is_confirmed || patch_url_text.isEmpty()) { return; }
        /* clang-format on */

        patches_page_ui_obj->list_widget->addItems(QStringList() << patch_url_text);

        // Apply flag to each item in list widget
        list_widget_apply_edit_flag(patches_page_ui_obj->list_widget);
    });

    patches_page_ui_obj->remove_entry_button->setIcon(QApplication::style()->standardIcon(QStyle::SP_TrashIcon));
    patches_page_ui_obj->move_up_button->setIcon(QApplication::style()->standardIcon(QStyle::SP_ArrowUp));
    patches_page_ui_obj->move_down_button->setIcon(QApplication::style()->standardIcon(QStyle::SP_ArrowDown));

    // remove entry
    connect(patches_page_ui_obj->remove_entry_button, &QPushButton::clicked, this, [patches_page_ui_obj]() {
        const auto current_index = patches_page_ui_obj->list_widget->currentRow();
        if (current_index < 0) {
            return;
        }
        delete patches_page_ui_obj->list_widget->takeItem(current_index);
    });

    // move up
    connect(patches_page_ui_obj->move_up_button, &QPushButton::clicked, this, [patches_page_ui_obj]() {
        const auto current_index = patches_page_ui_obj->list_widget->currentRow();
        if (current_index <= 0) {
            return;
        }
        auto* current_item = patches_page_ui_obj->list_widget->takeItem(current_index);
        patches_page_ui_obj->list_widget->insertItem(current_index - 1, current_item);
        patches_page_ui_obj->list_widget->setCurrentRow(current_index - 1);
    });
    // move down
    connect(patches_page_ui_obj->move_down_button, &QPushButton::clicked, this, [patches_page_ui_obj]() {
        const auto current_index = patches_page_ui_obj->list_widget->currentRow();
        if (current_index < 0 || current_index >= patches_page_ui_obj->list_widget->count() - 1) {
            return;
        }
        auto* current_item = patches_page_ui_obj->list_widget->takeItem(current_index);
        patches_page_ui_obj->list_widget->insertItem(current_index + 1, current_item);
        patches_page_ui_obj->list_widget->setCurrentRow(current_index + 1);
    });

    // Build completion polling (cycle-7 C3/D3): the bounded .done-status
    // retry after a marker-less helper exit (see on_done_status_tick).
    connect(&m_done_status_timer, &QTimer::timeout, this, &ConfWindow::on_done_status_tick);
}

void ConfWindow::closeEvent(QCloseEvent* event) {
    QWidget::closeEvent(event);
}

void ConfWindow::on_cancel() noexcept {
    close();
}

void ConfWindow::on_execute() noexcept {
    // Skip execution of the build, if already one is running
    /* clang-format off */
    if (m_running) { return; }
    /* clang-format on */
    m_running = true;

    auto* options_page_ui_obj = m_ui->conf_options_page_widget->get_ui_obj();
    auto* patches_page_ui_obj = m_ui->conf_patches_page_widget->get_ui_obj();

    sync_build_source();  // apply the user-selected build source first
    utils::prepare_build_environment();
    refresh_flavors();  // re-discover flavors from the refreshed source repo

    const std::int32_t main_combo_index = options_page_ui_obj->main_combo_box->currentIndex();
    const auto cpusched_path            = kernel_path_for_index(main_combo_index);
    if (cpusched_path.empty()) {
        m_running = false;
        fmt::print(stderr, "no kernel flavor available (source repo not prepared?)\n");
        return;
    }

    // Restore clean environment.
    const auto& all_set_values = get_all_set_values();
    utils::restore_clean_environment(m_previously_set_options, all_set_values);

    // Only files which end with .patch,
    // are considered as patches.
    const auto& orig_src_array = get_source_array_from_pkgbuild(cpusched_path, all_set_values);
    auto insert_status         = insert_new_source_array_into_pkgbuild(cpusched_path, patches_page_ui_obj->list_widget, orig_src_array);
    if (!insert_status) {
        m_running = false;
        fmt::print(stderr, "Failed to insert new source array into pkgbuild\n");
        return;
    }
    const auto& custom_name = options_page_ui_obj->custom_name_edit->text().toUtf8();
    insert_status           = set_custom_name_in_pkgbuild(cpusched_path, std::string_view{custom_name.constData(), static_cast<size_t>(custom_name.size())});
    if (!insert_status) {
        m_running = false;
        fmt::print(stderr, "Failed to set custom name in pkgbuild\n");
        return;
    }
    // The flavor path is absolute (resolved against the build repo root by
    // discover_repo_flavors) and doubles as the makepkg working directory.
    const auto& build_working_path = cpusched_path;

    // Run our build command!
    //
    // Source checksums are validated by default (no --skipchecksums): a
    // stale/mismatched source in the build repo fails the build until the
    // source is refreshed or the AUR package is updated. build_helper.sh
    // wraps makepkg: on "unknown public key" it imports the key (user
    // gpg keyring) and retries automatically.
    run_cmd_async(KM_HELPER_DIR "/build_helper.sh -scf --cleanbuild && touch .done-status", build_working_path);
}

void ConfWindow::on_save() noexcept {
    auto* options_page_ui_obj = m_ui->conf_options_page_widget->get_ui_obj();

    ConfigOptions config_options{};

    // checkboxes values (booleans)
    for (const auto& binding : checkbox_bindings) {
        config_options.*binding.config_field = checkstate_checked(options_page_ui_obj->*binding.widget);
    }

    // combobox values (strings that we try to find on load)
    config_options.hz_ticks_combo = get_hz_tick(static_cast<size_t>(options_page_ui_obj->hzticks_combo_box->currentIndex()));
    config_options.tickrate_combo = get_tickless_mode(static_cast<size_t>(options_page_ui_obj->tickless_combo_box->currentIndex()));
    config_options.preempt_combo  = get_preempt_mode(static_cast<size_t>(options_page_ui_obj->preempt_combo_box->currentIndex()));
    config_options.hugepage_combo = get_hugepage_mode(static_cast<size_t>(options_page_ui_obj->hugepage_combo_box->currentIndex()));
    config_options.lto_combo      = get_lto_mode(static_cast<size_t>(options_page_ui_obj->lto_combo_box->currentIndex()));
    config_options.cpu_opt_combo  = get_cpu_opt_mode(static_cast<size_t>(options_page_ui_obj->processor_opt_combo_box->currentIndex()));

    config_options.custom_name_edit = options_page_ui_obj->custom_name_edit->text().toStdString();

    auto save_file_path = QFileDialog::getSaveFileName(
        this,
        tr("Save file as"),
        QString::fromStdString(utils::fix_path("~/")),
        tr("Config file (*.toml)"))
                              .toStdString();
    /* clang-format off */
    if (save_file_path.empty()) { return; }
    /* clang-format on */

    if (!ConfigOptions::write_config_file(config_options, save_file_path)) {
        QMessageBox::critical(this, tr("Kernel Manager"), tr("Failed to save config options to file: %1").arg(QString::fromStdString(save_file_path)));
        return;
    }
}

void ConfWindow::on_load() noexcept {
    auto load_file_path = QFileDialog::getOpenFileName(
        this,
        tr("Load from"),
        QString::fromStdString(utils::fix_path("~/")),
        tr("Config file (*.toml)"))
                              .toStdString();
    /* clang-format off */
    if (load_file_path.empty()) { return; }
    /* clang-format on */

    auto config_options = ConfigOptions::parse_from_file(load_file_path);
    if (!config_options) {
        QMessageBox::critical(this, tr("Kernel Manager"), tr("Failed to load config options from file: %1").arg(QString::fromStdString(load_file_path)));
        return;
    }

    auto* options_page_ui_obj = m_ui->conf_options_page_widget->get_ui_obj();

    // checkboxes values (booleans)
    for (const auto& binding : checkbox_bindings) {
        set_checkstate(options_page_ui_obj->*binding.widget, (*config_options).*binding.config_field);
    }

    // combobox values (strings that we try to find on load)
    auto combobox_stat = set_combobox_val(options_page_ui_obj->hzticks_combo_box, lookup_hz_tick(config_options->hz_ticks_combo));
    combobox_stat += set_combobox_val(options_page_ui_obj->tickless_combo_box, lookup_tickless_mode(config_options->tickrate_combo));
    combobox_stat += set_combobox_val(options_page_ui_obj->preempt_combo_box, lookup_preempt_mode(config_options->preempt_combo));
    combobox_stat += set_combobox_val(options_page_ui_obj->hugepage_combo_box, lookup_hugepage_mode(config_options->hugepage_combo));
    combobox_stat += set_combobox_val(options_page_ui_obj->lto_combo_box, lookup_lto_mode(config_options->lto_combo));
    combobox_stat += set_combobox_val(options_page_ui_obj->processor_opt_combo_box, lookup_cpu_opt_mode(config_options->cpu_opt_combo));

    options_page_ui_obj->custom_name_edit->setText(QString::fromStdString(config_options->custom_name_edit));

    if (combobox_stat != 0) {
        QMessageBox::critical(this, tr("Kernel Manager"), tr("Config file(%1) is outdated").arg(QString::fromStdString(load_file_path)));
    }
}
