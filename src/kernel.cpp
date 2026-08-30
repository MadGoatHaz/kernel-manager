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

#include "kernel.hpp"
#include "aur_kernel.hpp"
#include "utils.hpp"

#include <cstdio>

#include <algorithm>        // for any_of, find_if
#include <filesystem>       // for exists
#include <initializer_list> // for initializer_list
#include <ranges>           // for ranges::*
#include <string>           // for string
#include <utility>          // for move

#include <fmt/compile.h>
#include <fmt/core.h>

namespace {

#ifdef ENABLE_AUR_KERNELS
static std::vector<std::string> g_aur_kernel_install_list{};  // NOLINT
#endif

static std::vector<std::string> g_kernel_install_list{};                             // NOLINT
static std::vector<std::string> g_kernel_removal_list{};                             // NOLINT
static const bool is_root_on_zfs = utils::exec("findmnt -ln -o FSTYPE /") == "zfs";  // NOLINT

// NOLINTNEXTLINE
static const bool has_chwd = [] {
    // Probe availability before use: chwd is distro-specific tooling and is
    // absent on a generic install, where detection must rely on alpm-DB checks only.
    const auto& probe = utils::exec("command -v chwd 2>/dev/null");
    return !probe.empty() && probe != "-1";
}();
// NOLINTNEXTLINE
static const bool is_nvidia_card_prebuild_module = [] {
    if (!has_chwd) {
        return false;
    }
    const auto& profile_names = utils::exec("chwd --list-installed -d 2>/dev/null | grep Name | awk '{print $4}'");
    return std::ranges::any_of(utils::make_split_view(profile_names, '\n'), [](auto&& profile_name) { return profile_name.starts_with("nvidia-dkms"); });
}();
// NOLINTNEXTLINE
static const bool is_nvidia_card_prebuild_open_module = [] {
    if (!has_chwd) {
        return false;
    }
    const auto& profile_names = utils::exec("chwd --list-installed -d 2>/dev/null | grep Name | awk '{print $4}'");
    return std::ranges::any_of(utils::make_split_view(profile_names, '\n'), [](auto&& profile_name) { return profile_name.starts_with("nvidia-open-dkms"); });
}();

// ---------------------------------------------------------------------------
// Data-driven module pairing, keyed by kernel base.
//
// A kernel package pairs with a module package derived as:
//     module = stem + (kernel_name with the kernel_base prefix removed) + suffix
// so a row whose stem equals its base reduces to "kernel_name + suffix"
// (e.g. base "foo" -> "foo-zfs", "foo-nvidia", "foo-nvidia-open"), while a row
// with a different stem re-prefixes the kernel's own suffix
// (e.g. "linux-lts" -> "nvidia-lts" / "nvidia-open-lts").
//
// The first row per kind whose kernel_base is a prefix of the kernel name
// wins, so more specific bases must precede more general ones. All module
// lookups (sync-DB pairing and installed-family queries) derive from this
// table; no per-distro package name patterns exist outside of it.
// ---------------------------------------------------------------------------
enum class ModuleKind { Zfs, Nvidia, NvidiaOpen };

struct KernelModuleSpec {
    std::string_view kernel_base;
    std::string_view module_stem;
    std::string_view module_suffix;
    ModuleKind kind;
};

static constexpr KernelModuleSpec kKernelModuleTable[] = {
    // family-based kernels: modules are suffixed onto the full kernel name
    {"linux-cachyos", "linux-cachyos", "-zfs", ModuleKind::Zfs},
    {"linux-cachyos", "linux-cachyos", "-nvidia", ModuleKind::Nvidia},
    {"linux-cachyos", "linux-cachyos", "-nvidia-open", ModuleKind::NvidiaOpen},
    // generic "linux" family kernels: modules are the stem plus the kernel's
    // suffix (linux -> nvidia, linux-lts -> nvidia-lts, ...)
    {"linux", "nvidia", "", ModuleKind::Nvidia},
    {"linux", "nvidia-open", "", ModuleKind::NvidiaOpen},
};

// Resolve the module package for a kernel name and kind (nullptr when absent).
static alpm_pkg_t* find_module_pkg(alpm_db_t* db, const std::string& kernel_name, ModuleKind kind) {
    for (const auto& spec : kKernelModuleTable) {
        if (spec.kind != kind || !kernel_name.starts_with(spec.kernel_base)) {
            continue;
        }
        std::string module_name{spec.module_stem};
        module_name += kernel_name.substr(spec.kernel_base.size());
        module_name += spec.module_suffix;
        return alpm_db_get_pkg(db, module_name.c_str());
    }
    return nullptr;
}

// Installed-module family regex for a kind, derived from the first (most
// specific) table row: "^<stem>.*<suffix>$".
static auto module_family_query(ModuleKind kind) {
    for (const auto& spec : kKernelModuleTable) {
        if (spec.kind == kind) {
            return fmt::format(FMT_COMPILE("^{}.*{}$"), spec.module_stem, spec.module_suffix);
        }
    }
    return std::string{};
}

}  // namespace

std::string Kernel::version() noexcept {
#ifdef ENABLE_AUR_KERNELS
    /* clang-format off */
    if (m_repo == "aur") { return m_version; }
    /* clang-format on */
#endif
    const char* sync_pkg_ver = alpm_pkg_get_version(m_pkg);
    /* clang-format off */
    if (!is_installed()) { return sync_pkg_ver; }
    /* clang-format on */

    auto* db                  = alpm_get_localdb(m_handle);
    auto* local_pkg           = alpm_db_get_pkg(db, m_name.c_str());
    const char* local_pkg_ver = alpm_pkg_get_version(local_pkg);
    const int32_t ret         = alpm_pkg_vercmp(local_pkg_ver, sync_pkg_ver);
    if (ret == 1) {
        return fmt::format(FMT_COMPILE("∨{}"), local_pkg_ver);
    } else if (ret == -1) {
        m_update = true;
        return fmt::format(FMT_COMPILE("∧{}"), sync_pkg_ver);
    }

    return sync_pkg_ver;
}

// Name must be without any repo name (e.g. core/linux)
bool Kernel::is_installed() const noexcept {
    auto* db  = alpm_get_localdb(m_handle);
    auto* pkg = alpm_db_get_pkg(db, m_name.c_str());

    return pkg != nullptr;
}

bool Kernel::install() const noexcept {
#ifdef ENABLE_AUR_KERNELS
    if (m_repo == "aur") {
        g_aur_kernel_install_list.push_back(m_name);
        return true;
    }
#endif
    const char* pkg_name    = alpm_pkg_get_name(m_pkg);
    const char* pkg_headers = alpm_pkg_get_name(m_headers);
    if (is_root_on_zfs && m_zfs_module != nullptr) {
        g_kernel_install_list.emplace_back(alpm_pkg_get_name(m_zfs_module));
    }

    const bool is_nvidia_dkms_installed = [handle = m_handle] {
        static constexpr std::string_view NVIDIA_DKMS_PKG = "nvidia-dkms";
        return alpm_db_get_pkg(alpm_get_localdb(handle), NVIDIA_DKMS_PKG.data()) != nullptr;
    }();
    const bool is_nvidia_open_dkms_installed = [handle = m_handle] {
        static constexpr std::string_view NVIDIA_OPEN_DKMS_PKG = "nvidia-open-dkms";
        return alpm_db_get_pkg(alpm_get_localdb(handle), NVIDIA_OPEN_DKMS_PKG.data()) != nullptr;
    }();
    const bool dkms_modules_not_installed = (!is_nvidia_dkms_installed && !is_nvidia_open_dkms_installed);

    // if we have any of the modules already installed,
    // then just use whatever is installed. skipping chwd detection
    // (queries derived from the module pairing table, not hard-coded regexes)
    const auto nvidia_family_query      = module_family_query(ModuleKind::Nvidia);
    const bool is_nvidia_modules_installed = !nvidia_family_query.empty() && !utils::exec(fmt::format(FMT_COMPILE("pacman -Qqs '{}' 2>/dev/null"), nvidia_family_query)).empty();
    const auto nvidia_open_family_query = module_family_query(ModuleKind::NvidiaOpen);
    const bool is_nvidia_open_modules_installed = !nvidia_open_family_query.empty() && !utils::exec(fmt::format(FMT_COMPILE("pacman -Qqs '{}' 2>/dev/null"), nvidia_open_family_query)).empty();

    bool should_install_nvidia      = (is_nvidia_card_prebuild_module && m_nvidia_module != nullptr);
    bool should_install_nvidia_open = (is_nvidia_card_prebuild_open_module && m_nvidia_open_module != nullptr);

    if (is_nvidia_open_modules_installed && m_nvidia_open_module != nullptr) {
        should_install_nvidia_open = true;
        should_install_nvidia      = false;
    } else if (is_nvidia_modules_installed && m_nvidia_module != nullptr) {
        should_install_nvidia_open = false;
        should_install_nvidia      = true;
    }

    if (dkms_modules_not_installed && should_install_nvidia_open) {
        g_kernel_install_list.emplace_back(alpm_pkg_get_name(m_nvidia_open_module));
    } else if (dkms_modules_not_installed && should_install_nvidia) {
        g_kernel_install_list.emplace_back(alpm_pkg_get_name(m_nvidia_module));
    }
    g_kernel_install_list.insert(g_kernel_install_list.end(), {pkg_name, pkg_headers});
    return true;
}

bool Kernel::remove() const noexcept {
    if (!is_installed()) {
        return false;
    }
    g_kernel_removal_list.push_back(m_name);

    const auto& append_to_removal_list = [this](alpm_pkg_t* sync_pkg) {
        if (sync_pkg == nullptr) {
            return;
        }

        const char* pkg_name = alpm_pkg_get_name(sync_pkg);

        // check if requested package is installed
        auto* db        = alpm_get_localdb(m_handle);
        auto* local_pkg = alpm_db_get_pkg(db, pkg_name);
        if (local_pkg != nullptr) {
            g_kernel_removal_list.emplace_back(pkg_name);
        }
    };

    append_to_removal_list(m_headers);
    append_to_removal_list(m_zfs_module);
    append_to_removal_list(m_nvidia_module);
    append_to_removal_list(m_nvidia_open_module);
    return true;
}

// Find kernel packages in every sync DB by the generic "linux*-headers"
// needle, which covers any distribution's kernel flavors (linux, linux-lts,
// AUR linux-zen, ...). For each header package the paired kernel package is
// resolved in the same DB, and the optional module packages (zfs/nvidia/
// nvidia-open) are paired via the data-driven module table (kKernelModuleTable).
// The "linux-api-headers" package is ignored (not a kernel header), and the
// sync DB named by KM_IGNORE_REPO (build-configurable, default: none) is
// skipped.

// The result consists of a list of reponame and a package name formatted as: "reponame/pkgname"
// For example:
//    reponame/linux-xxx reponame/linux-xxx-headers
//    reponame/linux-yyy reponame/linux-yyy-headers
//    ...
std::vector<Kernel> Kernel::get_kernels(alpm_handle_t* handle) noexcept {
    static constexpr std::string_view ignored_pkg  = "linux-api-headers";
    static constexpr std::string_view replace_part = "-headers";
    std::vector<Kernel> kernels{};

    auto* dbs                       = alpm_get_syncdbs(handle);
    [[maybe_unused]] auto* local_db = alpm_get_localdb(handle);
    for (alpm_list_t* i = dbs; i != nullptr; i = i->next) {
        static constexpr auto needle = "linux[^ ]*-headers";
        alpm_list_t* needles         = nullptr;
        alpm_list_t* ret_list        = nullptr;

        // NOLINTNEXTLINE
        needles = alpm_list_add(needles, const_cast<void*>(reinterpret_cast<const void*>(needle)));

        auto* db            = reinterpret_cast<alpm_db_t*>(i->data);
        const char* db_name = alpm_db_get_name(db);
        alpm_db_search(db, needles, &ret_list);

        for (alpm_list_t* j = ret_list; j != nullptr; j = j->next) {
            auto* pkg            = reinterpret_cast<alpm_pkg_t*>(j->data);
            std::string pkg_name = alpm_pkg_get_name(pkg);
            const auto& found    = std::ranges::search(pkg_name, ignored_pkg);
            if (!found.empty()) {
                continue;
            }
            alpm_pkg_t* headers = alpm_db_get_pkg(db, pkg_name.c_str());

            utils::remove_all(pkg_name, replace_part);
            pkg = alpm_db_get_pkg(db, pkg_name.c_str());

            // Skip if the actual kernel package is not found
            /* clang-format off */
            if (!pkg) { continue; }
            /* clang-format on */

            auto kernel_obj = Kernel{handle, pkg, headers, db_name, fmt::format(FMT_COMPILE("{}/{}"), db_name, pkg_name)};

#ifdef HAVE_ALPM_INSTALLED_DB
            auto* local_pkg = alpm_db_get_pkg(local_db, pkg_name.c_str());
            if (local_pkg) {
                const char* pkg_installed_db = alpm_pkg_get_installed_db(local_pkg);
                if (pkg_installed_db != nullptr) {
                    kernel_obj.m_installed_db = pkg_installed_db;
                }
            }
#endif
            kernel_obj.m_zfs_module         = find_module_pkg(db, pkg_name, ModuleKind::Zfs);
            kernel_obj.m_nvidia_module      = find_module_pkg(db, pkg_name, ModuleKind::Nvidia);
            kernel_obj.m_nvidia_open_module = find_module_pkg(db, pkg_name, ModuleKind::NvidiaOpen);

            kernels.emplace_back(std::move(kernel_obj));
        }

        alpm_list_free(needles);
        alpm_list_free(ret_list);
    }

#ifdef ENABLE_AUR_KERNELS
    namespace fs = std::filesystem;

    bool is_paru_installed{true};
    // accept both /sbin and /usr/bin locations (usr-merge systems)
    const auto exists_any = [](std::initializer_list<const char*> paths) {
        return std::ranges::any_of(paths, [](const char* path) { return fs::exists(path); });
    };
    if (!exists_any({"/sbin/paru", "/usr/bin/paru"}) || !exists_any({"/sbin/awk", "/usr/bin/awk"})) {
        fmt::print(stderr, "Paru and/or AWK are not installed! Disabling AUR kernels support\n");
        is_paru_installed = false;
    }

    if (!kernels.empty() && is_paru_installed) {
        auto&& aur_kernels_headers = utils::make_multiline(utils::exec("paru --aur -Sl | grep ' linux[^ ]*-headers' | awk '{print $2}'"));

        for (auto&& aur_kernel_header : aur_kernels_headers) {
            auto&& aur_kernel = std::string{aur_kernel_header};
            utils::replace_all(aur_kernel, "-headers", "");
            if (std::ranges::find_if(kernels, [&](auto& kernel) { return kernel.m_name == aur_kernel; }) != kernels.end()) {
                continue;
            }
            Kernel kernel_obj{};

            kernel_obj.m_handle       = handle;
            kernel_obj.m_repo         = "aur";
            kernel_obj.m_name         = aur_kernel;
            kernel_obj.m_name_headers = aur_kernel_header;
            kernel_obj.m_version      = "unknown-version";
            kernel_obj.m_raw          = fmt::format("aur/{}", aur_kernel);

            kernels.emplace_back(std::move(kernel_obj));
        }
    }
#endif

    return kernels;
}

void Kernel::commit_transaction() noexcept {
#ifdef ENABLE_AUR_KERNELS
    if (!g_aur_kernel_install_list.empty()) {
        detail::install_aur_kernels(g_aur_kernel_install_list);
        g_aur_kernel_install_list.clear();
    }
#endif
    if (!g_kernel_install_list.empty()) {
        const auto& packages_install = [&] { return utils::join_vec(g_kernel_install_list, " "); }();
        utils::runCmdTerminal(fmt::format(FMT_COMPILE("pacman -S --needed {}"), packages_install).c_str(), true);
    }

    if (!g_kernel_removal_list.empty()) {
        const auto& packages_remove = [&] { return utils::join_vec(g_kernel_removal_list, " "); }();
        utils::runCmdTerminal(fmt::format(FMT_COMPILE("pacman -Rsn {}"), packages_remove).c_str(), true);
    }
}

/** @brief Get global kernel install list
 *  @return Global kernel install list
 */
std::vector<std::string>& Kernel::get_install_list() noexcept {
    return g_kernel_install_list;
}

/** @brief Get global kernel removal list
 *  @return Global kernel removal list
 */
std::vector<std::string>& Kernel::get_removal_list() noexcept {
    return g_kernel_removal_list;
}
