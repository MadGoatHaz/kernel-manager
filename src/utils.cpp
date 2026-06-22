// Copyright (C) 2022-2026 Vladislav Nepogodin
//
// This file is part of CachyOS kernel manager.
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

#include <filesystem>  // for exists
#include <fstream>     // for ofstream

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
                  << "pkexec /usr/lib/cachyos-kernel-manager/rootshell.sh";
    }
    paramlist << cmd;

    proc.start("/usr/lib/cachyos-kernel-manager/terminal-helper", paramlist);
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

void prepare_build_environment() noexcept {
    static const fs::path app_path       = utils::fix_path("~/.cache/cachyos-km");
    static const fs::path pkgbuilds_path = utils::fix_path("~/.cache/cachyos-km/pkgbuilds");
    utils::prepare_git_repo(app_path, pkgbuilds_path, "https://github.com/cachyos/linux-cachyos.git");
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

}  // namespace utils
