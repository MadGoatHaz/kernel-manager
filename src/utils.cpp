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

#include "utils.hpp"

#include <cerrno>   // for errno
#include <cstdio>   // for fopen, fclose, fread, fseek, ftell, SEEK_END, SEEK_SET
#include <cstdlib>  // for system
#include <limits>   // for numeric_limits

#include <filesystem>  // for exists
#include <fstream>     // for ofstream

#include <fmt/compile.h>
#include <fmt/core.h>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wold-style-cast"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wnull-dereference"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wsuggest-attribute=pure"
#endif

#include <glib.h>

#include <QProcess>

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fs = std::filesystem;

namespace utils {

auto read_whole_file(std::string_view filepath) noexcept -> std::string {
    // Use std::fopen because it's faster than std::ifstream
    auto* file = std::fopen(filepath.data(), "rb");
    if (file == nullptr) {
        fmt::print(stderr, "[READWHOLEFILE] '{}' read failed: {}\n", filepath, std::strerror(errno));
        return {};
    }

    std::fseek(file, 0u, SEEK_END);
    const auto size = static_cast<std::size_t>(std::ftell(file));
    std::fseek(file, 0u, SEEK_SET);

    std::string buf;
    buf.resize(size);

    const std::size_t read = std::fread(buf.data(), sizeof(char), size, file);
    if (read != size) {
        fmt::print(stderr, "[READWHOLEFILE] '{}' read failed: {}\n", filepath, std::strerror(errno));
        std::fclose(file);
        return {};
    }
    std::fclose(file);

    return buf;
}

bool write_to_file(std::string_view filepath, std::string_view data) noexcept {
    std::ofstream file{std::string{filepath}};
    if (!file.is_open()) {
        fmt::print(stderr, "[WRITE_TO_FILE] '{}' open failed: {}\n", filepath, std::strerror(errno));
        return false;
    }
    file << data;
    return true;
}

// https://github.com/sheredom/subprocess.h
// https://gist.github.com/konstantint/d49ab683b978b3d74172
// https://github.com/arun11299/cpp-subprocess/blob/master/subprocess.hpp#L1218
// https://stackoverflow.com/questions/11342868/c-interface-for-interactive-bash
// https://github.com/hniksic/rust-subprocess
std::string exec(std::string_view command) noexcept {
    // NOLINTNEXTLINE
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(command.data(), "r"), pclose);
    if (!pipe) {
        fmt::print(stderr, "popen failed! '{}'\n", command);
        return "-1";
    }

    std::string result{};
    std::array<char, 128> buffer{};
    while (!feof(pipe.get())) {
        if (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
            result += buffer.data();
        }
    }

    if (result.ends_with('\n')) {
        result.pop_back();
    }

    return result;
}

int runCmdTerminal(QString cmd, bool escalate) noexcept {
    QProcess proc;
    cmd += "; read -p 'Press enter to exit'";
    auto paramlist = QStringList();
    if (escalate) {
        paramlist << "-s"
                  << "pkexec " KM_HELPER_DIR "/rootshell.sh";
    }
    paramlist << cmd;

    proc.start(KM_HELPER_DIR "/terminal-helper", paramlist);
    proc.waitForFinished(-1);
    return proc.exitCode();
}

int run_process(std::string_view program, const std::vector<std::string>& args) noexcept {
    QProcess proc;
    QStringList qargs{};
    for (const auto& arg : args) {
        qargs << QString::fromStdString(arg);
    }

    proc.setProcessChannelMode(QProcess::ForwardedChannels);
    proc.start(QString::fromStdString(std::string{program}), qargs);
    proc.waitForFinished(-1);
    if (proc.error() == QProcess::FailedToStart) {
        return -1;
    }
    return proc.exitCode();
}

std::string fix_path(std::string&& path) noexcept {
    /* clang-format off */
    if (path[0] != '~') { return std::move(path); }
    /* clang-format on */
    utils::replace_all(path, "~", g_get_home_dir());
    return std::move(path);
}

// Normalize a clone URL for comparison (drop a trailing '/' and '.git').
inline auto normalize_clone_url(std::string_view url) noexcept -> std::string {
    std::string result{url};
    while (result.ends_with('/')) {
        result.pop_back();
    }
    if (result.ends_with(".git")) {
        result.erase(result.size() - 4);
    }
    return result;
}

// A build source typed by the user: anything containing a scheme
// ("https://", "git://", "ssh://", ...) is a git URL used as-is, a bare name
// is resolved as an AUR package. No vendor URL is hard-coded.
inline auto build_source_clone_url(std::string_view source) noexcept -> std::string {
    if (source.find("://") != std::string_view::npos) {
        return std::string{source};
    }
    return fmt::format(FMT_COMPILE("https://aur.archlinux.org/{}.git"), source);
}

void prepare_git_repo(const fs::path& parent_dir, const fs::path& repo_path, std::string_view clone_url) noexcept {
    std::error_code ec{};

    const auto enter = [&ec](const fs::path& dir) {
        fs::current_path(dir, ec);
        if (ec) {
            fmt::print(stderr, "prepare_git_repo: cannot enter '{}': {}\n", dir.string(), ec.message());
        }
        return !ec;
    };

    fs::create_directories(parent_dir, ec);
    if (!enter(parent_dir)) {
        return;
    }

    if (fs::exists(repo_path, ec) && !fs::exists(repo_path / ".git", ec)) {
        fs::remove_all(repo_path, ec);
    }

    // A repo already cloned from a different source (the user switched the
    // build source): wipe it so the requested source is actually used.
    if (fs::exists(repo_path / ".git", ec)) {
        if (enter(repo_path)) {
            const auto origin = utils::exec("git remote get-url origin 2>/dev/null");
            enter(parent_dir);  // restore cwd before any removal
            if (!origin.empty() && normalize_clone_url(origin) != normalize_clone_url(clone_url)) {
                fmt::print("prepare_git_repo: source changed, replacing '{}' with '{}'\n", origin, clone_url);
                fs::remove_all(repo_path, ec);
                if (ec) {
                    fmt::print(stderr, "prepare_git_repo: cannot remove stale repo '{}': {}\n", repo_path.string(), ec.message());
                    return;
                }
            }
        }
    }

    if (!fs::exists(repo_path, ec)
        && run_process("git", {"clone", std::string{clone_url}, repo_path.filename().string()}) != 0) {
        fmt::print(stderr, "prepare_git_repo: 'git clone {}' failed\n", clone_url);
        return;
    }

    if (!enter(repo_path)) {
        return;
    }

    if (run_process("git", {"checkout", "--force", "master"}) != 0
        || run_process("git", {"clean", "-fd"}) != 0
        || run_process("git", {"pull"}) != 0) {
        fmt::print(stderr, "prepare_git_repo: failed to refresh checkout at '{}'\n", repo_path.string());
    }
}

const fs::path& build_repo_path() noexcept {
    static const fs::path pkgbuilds_path = utils::fix_path("~/.cache/kernel-manager/pkgbuilds");
    return pkgbuilds_path;
}

// Default build source: an AUR package name (resolves to the AUR git URL at
// runtime), not a hard-coded vendor clone URL.
namespace {
constexpr std::string_view kDefaultBuildSource = "linux-cachyos";
std::string g_build_source = std::string{kDefaultBuildSource};
}  // namespace

const std::string& build_source_repo() noexcept {
    return g_build_source;
}

void set_build_source_repo(std::string source) noexcept {
    // trim surrounding whitespace
    while (!source.empty() && (source.front() == ' ' || source.front() == '\t' || source.front() == '\n' || source.front() == '\r')) {
        source.erase(source.begin());
    }
    while (!source.empty() && (source.back() == ' ' || source.back() == '\t' || source.back() == '\n' || source.back() == '\r')) {
        source.pop_back();
    }
    if (source.empty()) {
        return;  // keep the current/default source
    }
    g_build_source = std::move(source);
}

void prepare_build_environment() noexcept {
    static const fs::path app_path = utils::fix_path("~/.cache/kernel-manager");
    utils::prepare_git_repo(app_path, build_repo_path(), build_source_clone_url(build_source_repo()));
}

void restore_clean_environment(std::vector<std::string>& previously_set_options, std::string_view all_set_values) noexcept {
    // Unset env variables before appplying new ones.
    for (auto&& previous_option : previously_set_options) {
        if (unsetenv(previous_option.c_str()) != 0) {
            fmt::print(stderr, "Cannot unset environment variable!: {}\n", std::strerror(errno));
        }
    }
    previously_set_options.clear();

    auto set_values_list = utils::make_split_view(all_set_values, '\n');
    for (auto&& expr : set_values_list) {
        auto expr_split = utils::make_multiline(std::move(expr), '=');
        auto var_name   = expr_split[0];

        const auto& var_val = expr_split[1];
        if (::setenv(var_name.c_str(), var_val.c_str(), 1) != 0) {
            fmt::print(stderr, "Cannot set environment variable!: {}\n", std::strerror(errno));
            continue;
        }

        // Save env name to unset it before running the next compilation.
        previously_set_options.emplace_back(std::move(var_name));
    }
}

namespace {

// Split content into lines, preserving empty lines (unlike
// utils::make_multiline, which drops them).
auto split_lines_preserving(std::string_view content) noexcept -> std::vector<std::string> {
    std::vector<std::string> lines{};
    std::size_t start = 0;
    while (start <= content.size()) {
        const auto pos = content.find('\n', start);
        if (pos == std::string_view::npos) {
            lines.emplace_back(content.substr(start));
            break;
        }
        lines.emplace_back(content.substr(start, pos - start));
        start = pos + 1;
    }
    return lines;
}

// Trim surrounding whitespace and one pair of surrounding quotes from a
// makepkg variable value (the right-hand side of an assignment line).
auto strip_pkgbuild_value(std::string_view value) noexcept -> std::string {
    const auto left  = value.find_first_not_of(" \t");
    const auto right = value.find_last_not_of(" \t");
    if (left == std::string_view::npos) {
        return std::string{};
    }
    value = value.substr(left, right - left + 1);
    if (value.size() >= 2) {
        const auto first = value.front();
        if ((first == '"' || first == '\'') && value.back() == first) {
            value = value.substr(1, value.size() - 2);
        }
    }
    return std::string{value};
}

// Locate the first top-level `var=` assignment line: either the plain
// column-0 form or the common AUR `true && var=` form. Returns the line
// index and the prefix (text before "var="); index == npos when absent.
auto find_top_level_var_line(const std::vector<std::string>& lines, std::string_view var) noexcept
    -> std::pair<std::size_t, std::string> {
    const std::string assign      = std::string{var} + "=";
    const std::string true_assign = "true && " + assign;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].starts_with(assign)) {
            return {i, std::string{}};
        }
        if (lines[i].starts_with(true_assign)) {
            return {i, "true && "};
        }
    }
    return {std::numeric_limits<std::size_t>::max(), std::string{}};
}

auto has_top_level_pkgver(const std::vector<std::string>& lines) noexcept -> bool {
    for (const auto& line : lines) {
        if (line.starts_with("pkgver=") || line.starts_with("pkgver()") || line.starts_with("true && pkgver=")
            || line.starts_with("true && pkgver()")) {
            return true;
        }
    }
    return false;
}

}  // namespace

auto apply_pkgbuild_custom_name(std::string content, std::string_view custom_name) noexcept -> PkgbuildRenameResult {
    // No rename requested: leave the content untouched.
    if (custom_name.empty() || custom_name == "$pkgbase") {
        return PkgbuildRenameResult{.ok = true, .new_content = std::move(content)};
    }

    // Only package-name characters plus the $pkgbase placeholder are allowed;
    // quotes, backticks and other shell metacharacters are rejected so the
    // rewritten line can never break out of the double quotes.
    for (const char c : custom_name) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                             || c == '_' || c == '.' || c == '+' || c == '-' || c == '$' || c == '{' || c == '}';
        if (!allowed) {
            return PkgbuildRenameResult{.ok = false, .error = fmt::format(FMT_COMPILE("custom package name '{}' contains invalid characters"), std::string{custom_name})};
        }
    }

    auto lines = split_lines_preserving(content);

    // Structural sanity: a recognizable PKGBUILD declares its version
    // top-level (pkgver= assignment or pkgver() function).
    if (!has_top_level_pkgver(lines)) {
        return PkgbuildRenameResult{.ok = false, .error = "no top-level pkgver= (or pkgver()) line: not a recognized PKGBUILD; file left untouched"};
    }

    // A custom package name binds to the first top-level pkgbase= line.
    const auto [pkgbase_index, pkgbase_prefix] = find_top_level_var_line(lines, "pkgbase");
    if (pkgbase_index == std::numeric_limits<std::size_t>::max()) {
        return PkgbuildRenameResult{.ok = false, .error = "no top-level pkgbase= assignment: cannot set a custom package name; file left untouched"};
    }

    // The original pkgbase value (right-hand side, quotes stripped) is what
    // a "$pkgbase" placeholder in the requested name expands to.
    const auto rhs      = std::string_view{lines[pkgbase_index]}.substr(pkgbase_prefix.size() + 8);  // "pkgbase="
    const auto original = strip_pkgbuild_value(rhs);

    std::string new_value{custom_name};
    utils::replace_all(new_value, "${pkgbase}", original);
    utils::replace_all(new_value, "$pkgbase", original);
    if (new_value.empty()) {
        return PkgbuildRenameResult{.ok = false, .error = "custom package name expands to an empty value"};
    }

    lines[pkgbase_index] = pkgbase_prefix + "pkgbase=\"" + new_value + "\"";
    return PkgbuildRenameResult{.ok = true, .new_content = utils::join_vec(lines, "\n")};
}

}  // namespace utils
