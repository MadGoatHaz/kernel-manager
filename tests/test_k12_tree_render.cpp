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
// glyph), Version cell (D4 "— (repo not enabled)" annotation), and the
// "Install from directory…" pseudo-row (C3, plan v1.24.0 — the appended
// 25th row: non-interactive folder-glyph QLabel, no checkbox, the D1
// tooltips, plus the menu probes below).
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
//   (4) DIRECTORY row (C3, plan v1.24.0): the "Install from directory…"
//       pseudo-row appended by init_kernels_tree_widget — no
//       ItemIsUserCheckable, a non-interactive folder-glyph QLabel in the
//       Check cell (non-null pixmap or the "📁" fallback), the D1
//       right-click tooltips on PkgName + Check, Version "—", Category
//       "Local", Install "—"; exactly one such row
//   (5) Regressions: the Installed column is "✓" iff the name is in the
//       local DB (the k11 relation); the PkgName tooltip is non-empty on
//       every data row; the row count equals the data total + the
//       directory row (25 on this machine = 24 data + 1 directory); no
//       duplicate PkgName
//   (6) Menu probes (the C5/E14 "never clicked" technique, via the tree's
//       public customContextMenuRequested signal — see the pre-include
//       note below; the app's own connection routes it to the same
//       on_kernel_context_menu handler a real right-click would trigger):
//       the directory row's menu = exactly ONE enabled action
//       "Install from directory…" (the action is NEVER clicked — no real
//       pkexec/terminal/folder-dialog launch); a live data row's menu
//       keeps the pre-C3 shape (3 or 4 actions) as the no-regression gate
// The K12-DUMP row dump must be byte-stable across two runs (deterministic
// order; the directory row dumps with the "folder" marker — checkbox rows
// "checkbox", lock rows "lock"); run_k12.sh checks that.
//
// The harness never writes: alpm is opened read-only (no root), no
// terminal is launched, no row is checked, the menu actions are never
// clicked (no folder dialog opens), /etc/pacman.conf is untouched.

// C3 (plan v1.24.0) menu probes: the spec's private->public pre-include
// hack (the C5/E14 throwaway pattern) is NOT used here — under this
// toolchain (GCC 16 libstdc++ + the project's full warning set) flipping
// access specifiers while km-window.hpp's transitive std includes parse
// is a hard error (std::basic_stringbuf::__xfer_bufptrs "redeclared with
// different access", -Wtemplate-body), and pre-including the whole
// transitive std set is brittle. Instead the probe EMITS the tree's
// PUBLIC customContextMenuRequested signal: the app's own ctor connection
// (km-window.cpp) routes it to the same MainWindow::on_kernel_context_menu
// handler a real right-click would trigger, so the probed flow is
// byte-identical to the user's — with no access-control acrobatics.
#include "kernel.hpp"         // Kernel::get_kernels (own ground truth)
#include "km-window.hpp"      // MainWindow (driven offscreen), TreeCol
#include "known_kernels.hpp"  // km::kernel_name_from_raw (row text -> name)
#include "utils.hpp"          // parse_alpm, release_alpm, alpm_root, alpm_libdir

#include <alpm.h>  // for alpm_*

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>  // for pair
#include <vector>   // for vector

#include <QAction>
#include <QApplication>
#include <QLabel>
#include <QMenu>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>

namespace {

// C3 (plan v1.24.0): the "Install from directory…" pseudo-row identity —
// mirrors km-window.cpp's file-local kDirectoryRowRaw (its tr() source
// literal). No translator is loaded in this harness, so the text the
// window renders equals this literal.
const QString kDirectoryRow = QStringLiteral("Install from directory…");

int g_failures       = 0;
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
    std::string raw{};          // "repo/name" — must equal the row's PkgName text
    std::string name{};         // bare kernel name (repo prefix stripped)
    std::string repo{};         // the row's repo (live: sync-DB name; info: curated install_repo)
    bool has_pkg = false;       // live (pkg present in a sync DB) vs curated info-row
    std::string version{};      // Kernel::version() (data layer — asserted on live cell text)
    bool installed    = false;  // direct local-DB probe (the k11 relation)
    bool repo_enabled = false;  // the repo is registered in the own handle's sync DBs
};

// The km-window.cpp file-local is_repo_in_syncdbs walk, independently
// re-derived for the ground truth (same alpm_get_syncdbs name match).
bool repo_in_syncdbs(alpm_handle_t* handle, std::string_view repo) {
    if (handle == nullptr) {
        return false;
    }
    for (alpm_list_t* i = alpm_get_syncdbs(handle); i != nullptr; i = i->next) {
        auto* db            = reinterpret_cast<alpm_db_t*>(i->data);
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
            e.raw                 = k.get_raw();
            e.name                = std::string{km::kernel_name_from_raw(k.get_raw())};
            e.repo                = std::string{k.get_repo()};
            e.has_pkg             = k.has_pkg();
            e.version             = k.version();
            e.installed           = localdb != nullptr && alpm_db_get_pkg(localdb, e.name.c_str()) != nullptr;
            e.repo_enabled        = repo_in_syncdbs(handle, k.get_repo());
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
    int live_rows                    = 0;
    int info_disabled                = 0;
    int info_enabled                 = 0;
    int directory_rows               = 0;
    QTreeWidgetItem* dir_item        = nullptr;  // the directory row (menu probe target)
    QTreeWidgetItem* first_live_item = nullptr;  // first live data row (no-regression menu probe)

    for (int r = 0; r < tree->topLevelItemCount(); ++r) {
        auto* item             = tree->topLevelItem(r);
        const QString pkg_raw  = item->text(static_cast<int>(TreeCol::PkgName));
        const std::string name = std::string{km::kernel_name_from_raw(pkg_raw.toStdString())};

        check(seen_raw.insert(pkg_raw.toStdString()).second, (std::string{"no duplicate PkgName " + pkg_raw.toStdString()}).c_str());

        // C3 (plan v1.24.0): the "Install from directory…" pseudo-row —
        // matched by its PkgName text (mirrors km-window.cpp's
        // kDirectoryRowRaw). It is NOT a kernel row (no expected entry),
        // so it takes its own assertion set and skips the data-row path
        // below.
        if (pkg_raw == kDirectoryRow) {
            ++directory_rows;
            check((item->flags() & Qt::ItemIsUserCheckable) != Qt::ItemIsUserCheckable,
                "directory row: no ItemIsUserCheckable (no checkbox)");
            check(!pkg_raw.isEmpty(), "directory row: PkgName non-empty");
            check(item->toolTip(static_cast<int>(TreeCol::PkgName)).contains(QStringLiteral("right-click")),
                "directory row: PkgName tooltip names the right-click flow");
            auto* dir_widget = tree->itemWidget(item, static_cast<int>(TreeCol::Check));
            check(dir_widget != nullptr, "directory row: Check cell has an item widget");
            if (dir_widget != nullptr) {
                auto* dir_label = qobject_cast<QLabel*>(dir_widget);
                check(dir_label != nullptr, "directory row: Check widget is a QLabel");
                if (dir_label != nullptr) {
                    // Visible folder glyph: a non-null theme pixmap or the
                    // centered "📁" text fallback (offscreen has no icon
                    // theme, so the fallback is the common case here).
                    const bool dir_glyph_visible = !dir_label->pixmap().isNull() || dir_label->text().contains(QStringLiteral("📁"));
                    check(dir_glyph_visible, "directory row: folder glyph visible (pixmap or 📁)");
                    check(dir_label->toolTip().contains(QStringLiteral("right-click")),
                        "directory row: Check tooltip names the right-click flow");
                }
            }
            check(item->text(static_cast<int>(TreeCol::Version)) == QStringLiteral("—"), "directory row: Version == —");
            check(item->text(static_cast<int>(TreeCol::Category)) == QStringLiteral("Local"), "directory row: Category == Local");
            check(item->text(static_cast<int>(TreeCol::Install)) == QStringLiteral("—"), "directory row: Install == —");
            if (dir_item == nullptr) {
                dir_item = item;
            }
            continue;
        }

        const auto it = expected.find(name);
        check(it != expected.end(), (std::string{"tree row maps to an expected kernel (" + name + ")"}).c_str());
        if (it == expected.end()) {
            continue;
        }
        covered.insert(name);
        const Expected& e = it->second;

        check(pkg_raw == QString::fromStdString(e.raw), (std::string{"PkgName text == expected raw " + e.raw}).c_str());
        check(!item->toolTip(static_cast<int>(TreeCol::PkgName)).isEmpty(), (std::string{"PkgName tooltip non-empty on " + e.raw}).c_str());

        // Regression (the k11 relation): the Installed column is "✓" iff
        // the name is in the local DB (live and info rows alike).
        const bool installed_cell = item->text(static_cast<int>(TreeCol::Install)) == QStringLiteral("✓");
        check(installed_cell == e.installed,
            (std::string{"Installed column "} + (e.installed ? "✓" : "—") + " iff in local DB (" + e.name + ")").c_str());

        auto* check_widget = tree->itemWidget(item, static_cast<int>(TreeCol::Check));

        if (e.has_pkg) {
            ++live_rows;
            if (first_live_item == nullptr) {
                first_live_item = item;
            }
            // Live row: the native checkbox — no item widget, user-checkable.
            check(check_widget == nullptr, (std::string{"live row: Check cell has no item widget (" + e.name + ")"}).c_str());
            check((item->flags() & Qt::ItemIsUserCheckable) == Qt::ItemIsUserCheckable, (std::string{"live row: ItemIsUserCheckable (" + e.name + ")"}).c_str());
            // The Version text is kernel.version() (∨/∧ update prefixes
            // tolerated — the data layer itself emits them).
            QString bare = item->text(static_cast<int>(TreeCol::Version));
            if (bare.startsWith(QStringLiteral("∨"))) {
                bare = bare.mid(1);
            } else if (bare.startsWith(QStringLiteral("∧"))) {
                bare = bare.mid(1);
            }
            check(bare == QString::fromStdString(e.version),
                (std::string{"live row: Version text == kernel.version() (" + e.name + ": cell " + item->text(static_cast<int>(TreeCol::Version)).toStdString() + ", data " + e.version + ")"}).c_str());
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
                check(item->text(static_cast<int>(TreeCol::Version)) == QStringLiteral("—"), (std::string{"info row (repo enabled): Version text == bare — (" + e.name + ")"}).c_str());
                check(item->toolTip(static_cast<int>(TreeCol::Version)).isEmpty(), (std::string{"info row (repo enabled): no Version tooltip (" + e.name + ")"}).c_str());
            } else {
                ++info_disabled;
                check(item->text(static_cast<int>(TreeCol::Version)) == QStringLiteral("— (repo not enabled)"),
                    (std::string{"info row (repo disabled): Version text == '— (repo not enabled)' (" + e.name + ")"}).c_str());
                const QString expected_ver_tip = QStringLiteral("Version unavailable — repo '%1' is not enabled. Right-click the row to add it.").arg(QString::fromStdString(e.repo));
                check(item->toolTip(static_cast<int>(TreeCol::Version)) == expected_ver_tip, (std::string{"info row (repo disabled): Version tooltip == D4 text (" + e.name + ")"}).c_str());
            }
        }
    }

    // Bidirectional coverage: every expected kernel has exactly one data
    // row (the directory row is not a kernel — it was excluded above).
    check(covered.size() == expected.size(),
        (std::string{"every expected kernel has a tree row (" + std::to_string(covered.size()) + " of " + std::to_string(expected.size()) + ")"}).c_str());

    // C3: exactly one directory row (the per-row duplicate check above
    // catches a second occurrence — this pins the count).
    check(directory_rows == 1, "the directory row appears exactly once");

    // Row count == the data total + the directory row (25 on this machine:
    // 24 data (17 live + 7 info) + 1 directory).
    check(static_cast<std::size_t>(tree->topLevelItemCount()) == expected.size() + std::size_t{1},
        (std::string{"row count == data rows + directory row (" + std::to_string(tree->topLevelItemCount()) + " = " + std::to_string(expected.size()) + " + 1)"}).c_str());

    std::printf("INFO: rows=%d total = %d live + %d info disabled-repo + %d info enabled-repo + %d directory\n", tree->topLevelItemCount(), live_rows, info_disabled, info_enabled, directory_rows);

    // ------------------------------------------------------------------
    // 4. Menu probes (C3, the C5/E14 "never clicked" technique): emit the
    //    tree's public customContextMenuRequested signal (the app's own
    //    ctor connection routes it to on_kernel_context_menu); a
    //    single-shot timer fires inside exec()'s nested event loop, scans
    //    the top-level widgets for the visible QMenu, records its
    //    actions, and dismisses it with NO selection (close() => exec()
    //    returns nullptr) — the action is never clicked, so no real
    //    pkexec/terminal/folder-dialog is launched. A hang guard (10 s)
    //    makes a never-shown menu fail cleanly instead of blocking the
    //    harness.
    // ------------------------------------------------------------------
    struct MenuProbe {
        std::vector<std::pair<std::string, bool>> actions{};  // (text, enabled)
        bool found = false;
    };
    auto probe_menu = [&](QTreeWidgetItem* row) -> MenuProbe {
        MenuProbe probe{};
        tree->setCurrentItem(row);

        // Hang guard: if the menu never becomes visible (a probe failure
        // mode), exit the nested exec() cleanly after 10 s — the
        // found=false checks below then fail the harness. The lambda takes
        // no arguments (the timeout signal carries none; qApp is the
        // context object, not a signal-argument source).
        QTimer hang_guard{};
        hang_guard.setSingleShot(true);
        hang_guard.setInterval(10000);
        QObject::connect(&hang_guard, &QTimer::timeout, qApp, [] { qApp->exit(3); });

        QTimer scan{};
        scan.setSingleShot(true);
        scan.setInterval(500);
        QObject::connect(&scan, &QTimer::timeout, [&probe, &hang_guard]() {
            hang_guard.stop();
            for (QWidget* w : qApp->topLevelWidgets()) {
                auto* m = qobject_cast<QMenu*>(w);
                if (m == nullptr || !m->isVisible()) {
                    continue;
                }
                for (QAction* a : m->actions()) {
                    probe.actions.emplace_back(a->text().toStdString(), a->isEnabled());
                }
                probe.found = true;
                m->close();  // dismiss with no selection => exec() returns nullptr
            }
        });
        scan.start();

        // A negative position hits no row => the handler's item-resolution
        // fallback is the current row (the one set above) — the
        // deterministic offscreen target. The direct connection (same
        // thread) runs the handler synchronously; it blocks in the menu's
        // exec() until the scan (or the hang guard) dismisses it.
        Q_EMIT tree->customContextMenuRequested(QPoint(-1000, -1000));
        hang_guard.stop();
        return probe;
    };

    // The directory row: exactly one action, enabled, the right text.
    check(dir_item != nullptr, "directory row found for the menu probe");
    if (dir_item != nullptr) {
        const MenuProbe dir_probe = probe_menu(dir_item);
        check(dir_probe.found, "directory row: context menu shown (single-shot probe)");
        check(dir_probe.actions.size() == std::size_t{1}, "directory row: exactly one context-menu action");
        if (dir_probe.actions.size() == std::size_t{1}) {
            check(dir_probe.actions[0].first == kDirectoryRow.toStdString(),
                "directory row: the one action is 'Install from directory…'");
            check(dir_probe.actions[0].second, "directory row: the one action is enabled");
        }
    }

    // A live data row: the pre-C3 menu shape (3 or 4 actions) — the
    // no-regression gate for the generic action build.
    check(first_live_item != nullptr, "a live data row exists for the no-regression menu probe");
    if (first_live_item != nullptr) {
        const MenuProbe live_probe = probe_menu(first_live_item);
        check(live_probe.found, "live row: context menu shown (no-regression probe)");
        if (live_probe.found) {
            const std::size_t live_n = live_probe.actions.size();
            check(live_n >= std::size_t{3} && live_n <= std::size_t{4},
                (std::string{"live row: pre-C3 menu shape (3 or 4 actions, got " + std::to_string(live_n) + ")"}).c_str());
            bool has_dir_action = false;
            for (const auto& a : live_probe.actions) {
                has_dir_action = has_dir_action || (a.first == kDirectoryRow.toStdString());
            }
            check(!has_dir_action, "live row: no directory-install action in the data-row menu");
        }
    }

    // ------------------------------------------------------------------
    // 5. The row dump (run_k12.sh requires it to be byte-stable across two
    //    runs — deterministic order; C3: the directory row dumps with the
    //    "folder" marker).
    // ------------------------------------------------------------------
    for (int r = 0; r < tree->topLevelItemCount(); ++r) {
        auto* item           = tree->topLevelItem(r);
        const bool checkable = (item->flags() & Qt::ItemIsUserCheckable) == Qt::ItemIsUserCheckable;
        const bool directory = item->text(static_cast<int>(TreeCol::PkgName)) == kDirectoryRow;
        std::printf("K12-DUMP: %s | %s | %s | %s | %s\n",
            item->text(static_cast<int>(TreeCol::PkgName)).toUtf8().constData(),
            item->text(static_cast<int>(TreeCol::Version)).toUtf8().constData(),
            item->text(static_cast<int>(TreeCol::Install)).toUtf8().constData(),
            item->text(static_cast<int>(TreeCol::Category)).toUtf8().constData(),
            checkable ? "checkbox" : (directory ? "folder" : "lock"));
    }

    utils::release_alpm(handle, &err);

    std::printf("K12: %zu checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        std::printf("K12: all assertions passed\n");
    }
    return g_failures;
}
