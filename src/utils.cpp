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
#include <QSettings>

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
// The popen pipe's RAII deleter (a struct — `decltype(&pclose)` as a
// template argument trips GCC's -Wignored-attributes on the function
// type's attributes; the struct form is the warning-free equivalent,
// behavior-identical: pclose runs iff the pointer is non-null).
struct pipe_deleter {
    void operator()(FILE* f) const noexcept {
        if (f != nullptr) {
            pclose(f);
        }
    }
};

std::string exec(std::string_view command) noexcept {
    std::unique_ptr<FILE, pipe_deleter> pipe(popen(command.data(), "r"));
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
    // Capture the command's exit code in __rc BEFORE the interactive read
    // prompt and exit the script with it: the trailing `read` is the last
    // command of the script line, and a script that merely runs `read`
    // ends with read's rc (0 — the user pressed Enter), masking a failed
    // command as success. `exit $__rc` terminates the line before the
    // helper's self-cleanup line (the rm -f safety net collects the temp
    // files — the k17 mid-script `exit` shape), so a blocking terminal
    // propagates the real rc back through the helper to this return.
    // (chunk D: the helper now blocks on a filesystem sentinel for every launch, so this exitCode is the command's real rc — not a launch contract — also for the non-blocking konsole/ptyxis terminals.)
    cmd += "; __rc=$?; read -p 'Press enter to exit'; exit $__rc";
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

    // Save the caller's CWD on entry; the guard below restores it on every
    // exit path (success and failure alike). Leaving the process inside the
    // cloned repo would make subsequent relative paths (e.g. the flavor
    // ".testscript" writes) resolve against the repo root.
    std::error_code cwd_ec{};
    const auto entry_cwd = fs::current_path(cwd_ec);

    struct CwdGuard {
        const fs::path cwd;
        ~CwdGuard() {
            if (cwd.empty()) {
                return;  // could not even read the entry CWD; nothing to restore
            }
            std::error_code restore_ec{};
            if (fs::equivalent(cwd, fs::current_path(restore_ec), restore_ec)) {
                return;  // already back where we started
            }
            fs::current_path(cwd, restore_ec);
            if (restore_ec) {
                fmt::print(stderr, "prepare_git_repo: cannot restore cwd '{}': {}\n", cwd.string(), restore_ec.message());
            }
        }
    } cwd_guard{entry_cwd};

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

// D4: the QSettings backing store for the user-selectable build directory.
// Explicit org/app (the main.cpp:126-129 identity) so the same file resolves
// in the app and in test harnesses, which redirect it deterministically via
// XDG_CONFIG_HOME. No cache: the accessors re-read on demand, so a
// mid-session set_build_dir() is visible immediately.
namespace {

    QSettings& app_settings() {
        static QSettings settings{"ArchLinux", "KernelManager"};
        return settings;
    }

}  // namespace

// The user-selectable build directory (QSettings "buildDir"), defaulting to
// the historical ~/.cache/kernel-manager/pkgbuilds location.
fs::path build_repo_path() noexcept {
    // toString().toStdString(): this Qt build's QVariant lacks toStdString()
    // (the QString side has it) — identical semantics.
    std::string dir = app_settings().value("buildDir").toString().toStdString();
    if (dir.empty()) {
        return utils::fix_path("~/.cache/kernel-manager/pkgbuilds");
    }
    return utils::fix_path(std::move(dir));
}

// The clone parent directory: the parent of build_repo_path().
fs::path build_app_path() noexcept {
    return build_repo_path().parent_path();
}

// Persist the user-selected build directory; trimmed, `~`-expanded, empty
// ⇒ no-op (the set_build_source_repo pattern).
void set_build_dir(std::string dir) noexcept {
    // trim surrounding whitespace
    while (!dir.empty() && (dir.front() == ' ' || dir.front() == '\t' || dir.front() == '\n' || dir.front() == '\r')) {
        dir.erase(dir.begin());
    }
    while (!dir.empty() && (dir.back() == ' ' || dir.back() == '\t' || dir.back() == '\n' || dir.back() == '\r')) {
        dir.pop_back();
    }
    if (dir.empty()) {
        return;  // keep the current value
    }
    app_settings().setValue("buildDir", QString::fromStdString(utils::fix_path(std::move(dir))));
}

// Default build source: an AUR package name (resolves to the AUR git URL at
// runtime), not a hard-coded vendor clone URL.
namespace {
    constexpr std::string_view kDefaultBuildSource = "linux-cachyos";
    std::string g_build_source                     = std::string{kDefaultBuildSource};
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
    const auto app_path = utils::build_app_path();
    utils::prepare_git_repo(app_path, utils::build_repo_path(), build_source_clone_url(build_source_repo()));
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

    // Split a custom name into the literal segments surrounding each
    // "$pkgbase" / "${pkgbase}" placeholder: segments.size() == 1 + the
    // placeholder count, and the name reassembles as
    // seg0 + PH + seg1 + PH + ... + segN. The two needle forms are
    // independent (the "${" of the braced form never matches "$pkgbase").
    auto split_on_pkgbase_placeholder(std::string_view name) noexcept -> std::vector<std::string_view> {
        std::vector<std::string_view> segments{};
        std::size_t pos = 0;
        for (;;) {
            const auto braced = name.find("${pkgbase}", pos);
            const auto bare   = name.find("$pkgbase", pos);
            if (braced == std::string_view::npos && bare == std::string_view::npos) {
                segments.push_back(name.substr(pos));
                return segments;
            }
            const bool take_braced = (braced != std::string_view::npos) && (bare == std::string_view::npos || braced < bare);
            const auto hit_pos     = take_braced ? braced : bare;
            const auto hit_len     = take_braced ? std::size_t{10} : std::size_t{8};
            segments.push_back(name.substr(pos, hit_pos - pos));
            pos = hit_pos + hit_len;
        }
    }

    // The inverse of the placeholder expansion: peel the wrapper layers off
    // `current`. Each previous run wrapped the then-current value as
    // seg0 + X + seg1 + X + ... + X + segN; if `current` matches, the inner X
    // is recovered and peeling continues (the user's …-custom-custom state is
    // two layers deep). Peeling stops at the first layer that does not match —
    // that value is the pre-rename base to expand against. With zero matching
    // layers, `current` is returned unchanged (a first-time application).
    // The rewrite therefore never grows the value: it lands on the same value
    // it started from, once every layer is peeled back.
    //
    // Termination is structural: a matching peel strictly shrinks the value
    // (the recovered X is a strict sub-part of it — its length is
    // (size − literal) / placeholder_count, which is < size whenever the
    // name carries any literal), so the loop cannot run more than
    // size / min(1, placeholder_count) peels. The only non-shrinking shape
    // is a name made of pure placeholder(s) (literal == 0 and a single
    // placeholder, e.g. the braced form "${pkgbase}"): there X == the whole
    // value and the peel is the identity, so the guard below breaks out and
    // `current` is returned unchanged — without it the loop would spin
    // forever (the pre-fix main-thread hang).
    auto unapply_pkgbuild_custom_name(std::string_view current, const std::vector<std::string_view>& segments) noexcept -> std::string {
        const std::size_t placeholder_count = segments.size() - 1;
        if (placeholder_count == 0) {
            return std::string{current};  // no placeholder: the rewrite is a pure line replacement, already idempotent
        }
        std::size_t literal = 0;
        for (const auto& segment : segments) {
            literal += segment.size();
        }

        std::string value{current};
        for (;;) {
            if (value.size() < literal || (value.size() - literal) % placeholder_count != 0) {
                break;
            }
            // The layout is fixed: the first X sits right after segments[0].
            const std::size_t x_len  = (value.size() - literal) / placeholder_count;
            const std::string_view x = std::string_view{value}.substr(segments[0].size(), x_len);
            if (x.size() >= value.size()) {
                break;  // pure-placeholder name: the peel is the identity and cannot
                        // shrink the value — stop, or the loop would spin forever
            }
            std::string candidate{};
            for (std::size_t i = 0; i < segments.size(); ++i) {
                candidate.append(segments[i]);
                if (i + 1 < segments.size()) {
                    candidate.append(x);
                }
            }
            if (candidate != value) {
                break;  // not a previous application of this name
            }
            value = std::string{x};
            if (x.empty()) {
                break;  // peeled down to nothing: the empty-value guard below flags it
            }
        }
        return value;
    }

}  // namespace

auto apply_pkgbuild_custom_name(std::string content, std::string_view custom_name) noexcept -> PkgbuildRenameResult {
    // No rename requested: leave the content untouched. Both placeholder
    // forms of the bare identity name count as "no rename" — the braced
    // form alone used to slip past this guard and spin the peel loop in
    // unapply_pkgbuild_custom_name forever.
    if (custom_name.empty() || custom_name == "$pkgbase" || custom_name == "${pkgbase}") {
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

    // Idempotency: the file's pkgbase may already carry a previous
    // application of this name (prepare_git_repo's checkout/clean failed
    // and the earlier rewrite survived). Expanding the placeholder
    // against that already-renamed value would re-wrap it (…-custom →
    // …-custom-custom); un-applying the wrapper recovers the pre-rename
    // base, so a repeat run lands on the same value instead of growing.
    const auto base = unapply_pkgbuild_custom_name(original, split_on_pkgbase_placeholder(custom_name));

    std::string new_value{custom_name};
    utils::replace_all(new_value, "${pkgbase}", base);
    utils::replace_all(new_value, "$pkgbase", base);
    if (new_value.empty()) {
        return PkgbuildRenameResult{.ok = false, .error = "custom package name expands to an empty value"};
    }

    lines[pkgbase_index] = pkgbase_prefix + "pkgbase=\"" + new_value + "\"";
    return PkgbuildRenameResult{.ok = true, .new_content = utils::join_vec(lines, "\n")};
}

}  // namespace utils
