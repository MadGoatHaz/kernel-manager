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

// k12: offscreen render harness for the kernel tree's Choose cell (D3 lock
// glyph) and Version cell (D4 "— (repo not enabled)" annotation).
//
// Follows the tests/run_chunk2_ui.sh recipe: compiles the REAL source
// closure of the app (km-window.cpp + conf-window.cpp + kernel.cpp +
// known_kernels.cpp + alpm_utils.cpp + utils.cpp + install_kernel.cpp +
// aur_kernel.cpp + boot_instructions.cpp + bootloader.cpp +
// config-options.cpp + their AUTOMOC mocs) with the project's GCC warning
// set, links fmt + the cxxbridge Rust libs + glib + Qt6 + libalpm, and
// drives the real MainWindow offscreen (run via tests/run_k12.sh).
//
// The ground truth is recomputed from the public surface (k11 style — no
// hard-coded counts, environment-tolerant):
//   - Kernel::get_kernels on an OWN parse_alpm handle, per name
//   - a sync-DB walk on that handle (is the repo enabled?)
//   - a local-DB walk (is the package installed?)
// and asserted per tree row (mapped by its PkgName text "repo/name"):
//   (1) LIVE row (has_pkg): Check cell has NO item widget (the native
//       checkbox), the row is user-checkable, and the Version text is
//       kernel.version() (∨/∧ update prefixes tolerated)
//   (2) INFO row, repo DISABLED: Check cell hosts a QLabel with a visible
//       lock glyph (non-null pixmap or the "🔒" fallback) + the D3 tooltip
//       "Repo '<r>' not enabled — right-click the row to add it"; Version
//       text is the D4 annotation "— (repo not enabled)" + the exact
//       non-empty Version tooltip
//   (3) INFO row, repo ENABLED (the E14 `pacman -Sy` case — vacuous on
//       this machine, the relation is still asserted): lock QLabel + D3
//       tooltip "Not in the '<r>' DB — run `pacman -Sy`"; Version text is
//       the bare "—" with no Version tooltip
//   (4) Regressions: the Installed column is "✓" iff the name is in the
//       local DB (the k11 relation); the PkgName tooltip is non-empty on
//       every row; the row count equals the expected total (20 here =
//       6 live + 14 info); no duplicate PkgName
// The K12-DUMP row dump must be byte-stable across two runs (deterministic
// order); run_k12.sh checks that.
//
// The harness never writes: alpm is opened read-only (no root), no
// terminal is launched, no row is checked, /etc/pacman.conf is untouched.

#include "km-window.hpp"     // MainWindow (driven offscreen), TreeCol
#include "kernel.hpp"        // Kernel::get_kernels (own ground truth)
#include "known_kernels.hpp" // km::kernel_name_from_raw (row text -> name)
#include "utils.hpp"         // parse_alpm, release_alpm, alpm_root, alpm_libdir

#include <alpm.h>  // for alpm_*

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <thread>

#include <QApplication>
#include <QLabel>
#include <QTreeWidget>
#include <QTreeWidgetItem>

namespace {

int g_failures = 0;
std::size_t g_checks = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    if (condition) {
        std::printf("PASS: %s\n", what);
    } else {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// The ground-truth view of one expected kernel row, derived from the
// harness's own handle (never from the window's internals).
struct Expected {
    std::string raw{};         // "repo/name" — must equal the row's PkgName text
    std::string name{};        // bare kernel name (repo prefix stripped)
    std::string repo{};        // the row's repo (live: sync-DB name; info: curated install_repo)
    bool has_pkg = false;      // live (pkg present in a sync DB) vs curated info-row
    std::string version{};     // Kernel::version() (data layer — asserted on live cell text)
    bool installed = false;    // direct local-DB probe (the k11 relation)
    bool repo_enabled = false; // the repo is registered in the own handle's sync DBs
};

// The km-window.cpp file-local is_repo_in_syncdbs walk, independently
// re-derived for the ground truth (same alpm_get_syncdbs name match).
bool repo_in_syncdbs(alpm_handle_t* handle, std::string_view repo) {
    if (handle == nullptr) {
        return false;
    }
    for (alpm_list_t* i = alpm_get_syncdbs(handle); i != nullptr; i = i->next) {
        auto* db = reinterpret_cast<alpm_db_t*>(i->data);
        const char* db_name = alpm_db_get_name(db);
        if (db_name != nullptr && repo == db_name) {
            return true;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app{argc, argv};

    // ------------------------------------------------------------------
    // 1. Ground truth from the public surface (own alpm handle).
    // ------------------------------------------------------------------
    alpm_errno_t err{};
    auto* handle = utils::parse_alpm(utils::alpm_root, utils::alpm_libdir, &err);
    check(handle != nullptr, "own alpm handle created (read-only, no root)");
    if (handle == nullptr) {
        std::printf("K12: FATAL — no alpm handle, cannot probe the DBs\n");
        std::printf("K12: %zu checks, %d failures\n", g_checks, g_failures);
        return 2;
    }

    std::map<std::string, Expected> expected{};  // keyed by the bare name
    auto* localdb = alpm_get_localdb(handle);
    check(localdb != nullptr, "local DB present (alpm_get_localdb)");
    {
        auto kernels = Kernel::get_kernels(handle);
        for (auto& k : kernels) {  // version() is non-const (it marks m_update)
            Expected e{};
            e.raw          = k.get_raw();
            e.name         = std::string{km::kernel_name_from_raw(k.get_raw())};
            e.repo         = std::string{k.get_repo()};
            e.has_pkg      = k.has_pkg();
            e.version      = k.version();
            e.installed    = localdb != nullptr && alpm_db_get_pkg(localdb, e.name.c_str()) != nullptr;
            e.repo_enabled = repo_in_syncdbs(handle, k.get_repo());
            const std::string key = e.name;  // copied before the move below (argument evaluation order)
            std::string dup_msg   = "ground truth: no duplicate kernel name " + key;
            const bool inserted   = expected.emplace(key, std::move(e)).second;
            check(inserted, dup_msg.c_str());
        }
    }
    std::printf("INFO: expected %zu kernel rows from the public surface (own handle + sync-DB + local-DB walks)\n", expected.size());

    // ------------------------------------------------------------------
    // 2. The real window. Its ctor populates the tree (the deferred init
    //    is waited on inside the ctor); the pump below is a timeout-guarded
    //    belt-and-braces for any queued meta-calls.
    // ------------------------------------------------------------------
    MainWindow window;

    QTreeWidget* tree = window.findChild<QTreeWidget*>("treeKernels");
    check(tree != nullptr, "treeKernels found on the real MainWindow");
    if (tree == nullptr) {
        std::printf("K12: FATAL — no treeKernels widget\n");
        std::printf("K12: %zu checks, %d failures\n", g_checks, g_failures);
        utils::release_alpm(handle, &err);
        return 2;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (tree->topLevelItemCount() == 0 && std::chrono::steady_clock::now() < deadline) {
        app.processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(tree->topLevelItemCount() > 0, "rows landed in the tree (timeout-guarded)");

    // ------------------------------------------------------------------
    // 3. Row-level assertions (mapped by the PkgName text).
    // ------------------------------------------------------------------
    std::set<std::string> seen_raw{};
    std::set<std::string> covered{};  // expected names with a matching tree row
    int live_rows     = 0;
    int info_disabled = 0;
    int info_enabled  = 0;

    for (int r = 0; r < tree->topLevelItemCount(); ++r) {
        auto* item = tree->topLevelItem(r);
        const QString pkg_raw = item->text(TreeCol::PkgName);
        const std::string name = std::string{km::kernel_name_from_raw(pkg_raw.toStdString())};

        check(seen_raw.insert(pkg_raw.toStdString()).second, (std::string{"no duplicate PkgName " + pkg_raw.toStdString()}).c_str());

        const auto it = expected.find(name);
        check(it != expected.end(), (std::string{"tree row maps to an expected kernel (" + name + ")"}).c_str());
        if (it == expected.end()) {
            continue;
        }
        covered.insert(name);
        const Expected& e = it->second;

        check(pkg_raw == QString::fromStdString(e.raw), (std::string{"PkgName text == expected raw " + e.raw}).c_str());
        check(!item->toolTip(TreeCol::PkgName).isEmpty(), (std::string{"PkgName tooltip non-empty on " + e.raw}).c_str());

        // Regression (the k11 relation): the Installed column is "✓" iff
        // the name is in the local DB (live and info rows alike).
        const bool installed_cell = item->text(TreeCol::Install) == QStringLiteral("✓");
        check(installed_cell == e.installed,
              (std::string{"Installed column "} + (e.installed ? "✓" : "—") + " iff in local DB (" + e.name + ")").c_str());

        auto* check_widget = tree->itemWidget(item, TreeCol::Check);

        if (e.has_pkg) {
            ++live_rows;
            // Live row: the native checkbox — no item widget, user-checkable.
            check(check_widget == nullptr, (std::string{"live row: Check cell has no item widget (" + e.name + ")"}).c_str());
            check((item->flags() & Qt::ItemIsUserCheckable) == Qt::ItemIsUserCheckable, (std::string{"live row: ItemIsUserCheckable (" + e.name + ")"}).c_str());
            // The Version text is kernel.version() (∨/∧ update prefixes
            // tolerated — the data layer itself emits them).
            QString bare = item->text(TreeCol::Version);
            if (bare.startsWith(QStringLiteral("∨"))) {
                bare = bare.mid(1);
            } else if (bare.startsWith(QStringLiteral("∧"))) {
                bare = bare.mid(1);
            }
            check(bare == QString::fromStdString(e.version),
                  (std::string{"live row: Version text == kernel.version() (" + e.name + ": cell " + item->text(TreeCol::Version).toStdString() + ", data " + e.version + ")"}).c_str());
        } else {
            // Info-row: the non-interactive lock QLabel in the Check cell (D3).
            check(check_widget != nullptr, (std::string{"info row: Check cell has an item widget (" + e.name + ")"}).c_str());
            if (check_widget != nullptr) {
                auto* label = qobject_cast<QLabel*>(check_widget);
                check(label != nullptr, (std::string{"info row: Check widget is a QLabel (" + e.name + ")"}).c_str());
                if (label != nullptr) {
                    // Visible glyph: a non-null theme pixmap or the centered
                    // "🔒" text fallback (offscreen has no icon theme, so
                    // the fallback is the common case here).
                    const bool glyph_visible = !label->pixmap().isNull() || label->text().contains(QStringLiteral("🔒"));
                    check(glyph_visible, (std::string{"info row: lock glyph visible (pixmap or 🔒) (" + e.name + ")"}).c_str());
                    // The exact D3 two-case tooltip (repo name from the
                    // row's own repo, as the window renders it).
                    const QString expected_tip = e.repo_enabled
                        ? QStringLiteral("Not in the '%1' DB — run `pacman -Sy`").arg(QString::fromStdString(e.repo))
                        : QStringLiteral("Repo '%1' not enabled — right-click the row to add it").arg(QString::fromStdString(e.repo));
                    check(label->toolTip() == expected_tip, (std::string{"info row: lock tooltip == D3 two-case text (" + e.name + ")"}).c_str());
                }
            }
            // The Version cell per the D4 annotation.
            if (e.repo_enabled) {
                ++info_enabled;
                check(item->text(TreeCol::Version) == QStringLiteral("—"), (std::string{"info row (repo enabled): Version text == bare — (" + e.name + ")"}).c_str());
                check(item->toolTip(TreeCol::Version).isEmpty(), (std::string{"info row (repo enabled): no Version tooltip (" + e.name + ")"}).c_str());
            } else {
                ++info_disabled;
                check(item->text(TreeCol::Version) == QStringLiteral("— (repo not enabled)"),
                      (std::string{"info row (repo disabled): Version text == '— (repo not enabled)' (" + e.name + ")"}).c_str());
                const QString expected_ver_tip = QStringLiteral("Version unavailable — repo '%1' is not enabled. Right-click the row to add it.").arg(QString::fromStdString(e.repo));
                check(item->toolTip(TreeCol::Version) == expected_ver_tip, (std::string{"info row (repo disabled): Version tooltip == D4 text (" + e.name + ")"}).c_str());
            }
        }
    }

    // Bidirectional coverage: every expected kernel has exactly one row.
    check(covered.size() == expected.size(),
          (std::string{"every expected kernel has a tree row (" + std::to_string(covered.size()) + " of " + std::to_string(expected.size()) + ")"}).c_str());

    // Row count == the expected total (20 on this machine: 6 live + 14 info).
    check(static_cast<std::size_t>(tree->topLevelItemCount()) == expected.size(),
          (std::string{"row count == expected total (" + std::to_string(tree->topLevelItemCount()) + " = " + std::to_string(expected.size()) + ")"}).c_str());

    std::printf("INFO: rows=%d total = %d live + %d info disabled-repo + %d info enabled-repo\n", tree->topLevelItemCount(), live_rows, info_disabled, info_enabled);

    // ------------------------------------------------------------------
    // 4. The row dump (run_k12.sh requires it to be byte-stable across two
    //    runs — deterministic order).
    // ------------------------------------------------------------------
    for (int r = 0; r < tree->topLevelItemCount(); ++r) {
        auto* item = tree->topLevelItem(r);
        const bool checkable = (item->flags() & Qt::ItemIsUserCheckable) == Qt::ItemIsUserCheckable;
        std::printf("K12-DUMP: %s | %s | %s | %s | %s\n",
                    item->text(TreeCol::PkgName).toUtf8().constData(),
                    item->text(TreeCol::Version).toUtf8().constData(),
                    item->text(TreeCol::Install).toUtf8().constData(),
                    item->text(TreeCol::Category).toUtf8().constData(),
                    checkable ? "checkbox" : "lock");
    }

    utils::release_alpm(handle, &err);

    std::printf("K12: %zu checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("K12: all assertions passed\n");
    }
    return g_failures;
}
