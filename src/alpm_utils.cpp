// Copyright (C) 2022-2025 Vladislav Nepogodin
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

#include "alpm_utils.hpp"
#include "ini.hpp"
#include "known_kernels.hpp"  // for km::known_kernels (the curated install_repo set)
#include "utils.hpp"          // for utils::exec (AUR probe executor)

namespace utils {

// Sync repo excluded from kernel discovery. Build-configurable via the
// KM_IGNORE_REPO CMake variable (default: empty = no exclusion), so no repo
// name is hard-coded in the source.
#ifndef KM_IGNORE_REPO
#define KM_IGNORE_REPO ""
#endif

// Install prefix for the pkexec helpers (terminal-helper, rootshell.sh,
// repo_add.sh). Defined by CMake (the single KM_HELPER_DIR source of
// truth); the fallback mirrors the KM_IGNORE_REPO pattern so standalone
// compile contexts (the k7-style harnesses) build without it.
#ifndef KM_HELPER_DIR
#define KM_HELPER_DIR "/usr/lib/kernel-manager"
#endif

namespace {

    // Character set of a valid pacman repo name: lowercase alphanumerics and
    // dashes. Enforced before the name is embedded in the repo_add.sh command
    // line — the single quote makes any violation inert, and the curated-table
    // guard is the last wall.
    [[gnu::pure]] [[nodiscard]] bool valid_repo_chars(std::string_view repo) noexcept {
        for (const char c : repo) {
            if ((c < 'a' || c > 'z') && (c < '0' || c > '9') && c != '-') {
                return false;
            }
        }
        return true;
    }

    // Whether the repo appears as an install_repo value in the curated kernel
    // table — the set of repos kernels are pre-compiled against. core/extra
    // pass this guard (they are table repos) but are base-install repos that
    // the repo_add.sh allowlist rejects; the script is the final wall.
    [[nodiscard]] bool is_known_repo(std::string_view repo) noexcept {
        for (const auto& k : km::known_kernels()) {
            if (k.install_repo == repo) {
                return true;
            }
        }
        return false;
    }

}  // namespace

alpm_handle_t* parse_alpm(std::string_view root, std::string_view dbpath, alpm_errno_t* err) noexcept {
    // Initialize alpm.
    alpm_handle_t* alpm_handle = alpm_initialize(std::string{root}.c_str(), std::string{dbpath}.c_str(), err);
    if (alpm_handle == nullptr) {
        return nullptr;
    }

    // Parse pacman config.
    static constexpr auto pacman_conf_path = "/etc/pacman.conf";
    static constexpr auto ignored_repo     = KM_IGNORE_REPO;

    const mINI::INIFile file(pacman_conf_path);
    // next, create a structure that will hold data
    mINI::INIStructure ini;

    // now we can read the file
    file.read(ini);
    for (const auto& it : ini) {
        const auto& section = it.first;
        if (section == ignored_repo || section == "options") {
            continue;
        }
        [[maybe_unused]] auto* db = alpm_register_syncdb(alpm_handle, section.c_str(), ALPM_SIG_USE_DEFAULT);
    }

    return alpm_handle;
}

std::int32_t release_alpm(alpm_handle_t* handle, alpm_errno_t* err) noexcept {
    // read errno before
    if (err != nullptr) {
        *err = alpm_errno(handle);
    }
    // Release libalpm handle
    return alpm_release(handle);
}

bool is_aur_package_available(const ExecFn& exec_fn, std::string_view pkg) noexcept {
    // The AUR can only be queried through paru; without it the package is
    // reported as unavailable (the custom build remains the install path).
    const std::string paru_path = exec_fn("command -v paru");
    if (paru_path.empty() || paru_path == "-1") {
        return false;
    }

    // `paru --aur -Si` prints the package metadata to stdout when the
    // package exists and stays silent on stdout (error on stderr) when it
    // does not. The name is single-quoted: pacman package names contain no
    // shell metacharacters.
    const std::string info = exec_fn("paru --aur -Si '" + std::string{pkg} + "'");
    return !info.empty() && info != "-1";
}

bool is_package_in_sync_db(alpm_handle_t* handle, std::string_view repo, std::string_view pkg) noexcept {
    if (handle == nullptr) {
        return false;
    }
    // Locate the sync DB registered under `repo` (parse_alpm registers
    // every non-ignored /etc/pacman.conf section); a missing section means
    // the repo is not added on this system.
    for (const alpm_list_t* i = alpm_get_syncdbs(handle); i != nullptr; i = i->next) {
        auto* db            = reinterpret_cast<alpm_db_t*>(i->data);
        const char* db_name = alpm_db_get_name(db);
        if (db_name == nullptr || std::string_view{db_name} != repo) {
            continue;
        }
        return alpm_db_get_pkg(db, std::string{pkg}.c_str()) != nullptr;
    }
    return false;
}

bool is_package_available(std::string_view pkg, std::string_view repo) noexcept {
    if (classify_repo(repo) == PackageSource::AUR) {
        // Inject utils::exec (popen-based, "-1" on spawn failure) as the
        // AUR probe executor.
        const ExecFn exec_fn{[](std::string_view command) noexcept { return utils::exec(command); }};
        return is_aur_package_available(exec_fn, pkg);
    }

    // Fresh handle per call: read-only sync-DB access, no root, safe to
    // invoke repeatedly (released on the way out).
    alpm_errno_t err{};
    alpm_handle_t* handle = parse_alpm(alpm_root, alpm_libdir, &err);
    if (handle == nullptr) {
        return false;
    }
    const bool available = is_package_in_sync_db(handle, repo, pkg);
    release_alpm(handle, &err);
    return available;
}

bool is_repo_enabled(std::string_view repo, std::string_view conf_path) noexcept {
    // The same mINI read as parse_alpm's sync-DB registration: an active
    // (uncommented) [repo] section lands in the structure, a commented-out
    // one never does.
    const mINI::INIFile file(conf_path);
    mINI::INIStructure ini;
    file.read(ini);
    for (const auto& it : ini) {
        if (it.first == repo) {
            return true;
        }
    }
    return false;
}

int add_repo_to_pacman_conf(std::string_view repo, RepoCmdRunner runner) noexcept {
    // Guard: reject without ever calling the runner (k8 CommandRunner
    // precedent) — an empty name, "aur" (the AUR is not a pacman repo), a
    // character-set violation, or a repo outside the curated table's
    // install_repo set.
    if (repo.empty() || repo == "aur" || !valid_repo_chars(repo) || !is_known_repo(repo)) {
        return -1;
    }

    // A null runner selects the real one: the unified polkit privilege
    // layer (an escalated command is wrapped in pkexec rootshell — see
    // utils.cpp).
    if (!runner) {
        runner = [](std::string_view cmd, bool escalate) noexcept {
            return utils::runCmdTerminal(QString::fromUtf8(cmd.data()), escalate);
        };
    }

    // Single-quoted: the charset guard above makes the quote inert (no
    // char can break out of the argument), and the script's own allowlist
    // is the final wall.
    std::string cmd{KM_HELPER_DIR};
    cmd += "/repo_add.sh '";
    cmd.append(repo);
    cmd += "'";
    return runner(cmd, true);
}

}  // namespace utils
