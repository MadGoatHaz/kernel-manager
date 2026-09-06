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

#ifndef MAINWINDOW_HPP_
#define MAINWINDOW_HPP_

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wfloat-conversion"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wimplicit-int-float-conversion"
#pragma clang diagnostic ignored "-Wdeprecated-enum-enum-conversion"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#elifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wdeprecated-enum-enum-conversion"
#pragma GCC diagnostic ignored "-Wsuggest-attribute=pure"
#pragma GCC diagnostic ignored "-Wsuggest-final-types"
#pragma GCC diagnostic ignored "-Wsuggest-final-methods"
#endif

#include <ui_km-window.h>

#include "conf-window.hpp"
#include "driver_gate.hpp"
#include "kernel.hpp"
#include "kernel_info.hpp"
#include "utils.hpp"

#ifdef WITH_SCX_MANAGER
#include <scx-manager/schedext-window.hpp>
#endif

#include <array>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <alpm.h>

#include <QFutureWatcher>
#include <QMainWindow>
#include <QPoint>
#include <QProgressBar>
#include <QProgressDialog>
#include <QThread>
#include <QTimer>

#ifdef __clang__
#pragma clang diagnostic pop
#elifdef __GNUC__
#pragma GCC diagnostic pop
#endif

class Work final : public QObject {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(Work)

 public:
    using function_t = std::function<void()>;
    explicit Work(function_t&& func)
      : m_func(std::move(func)) { }
    ~Work() override = default;

    void doHeavyCalculations();

 private:
    function_t m_func;
};

// Column indices of the kernel tree (a scoped enum: the Qt APIs take
// int columns, so call sites pass static_cast<int>(TreeCol::…)).
enum class TreeCol : std::uint8_t { Check,
    PkgName,
    Version,
    Category,
    Install,  // K10: installed-on-system indicator ("✓" / "—") from the alpm local DB, read-only; availability is the context-menu's concern
    Immutable };

class KernelTreeWidgetItem : public QTreeWidgetItem {
 public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem& other) const override;
};

// The file-local resize-time ellipsis filter (plan v1.30.0 D5) for the
// long-string labels — defined in km-window.cpp at global scope right
// after the anonymous namespace (a namespaced definition would be a
// distinct class this declaration could not name); the member below is
// a raw pointer to its window-parented instance, so only the
// declaration is needed here (the definition never leaves the .cpp).
class ElideFilter;

class MainWindow final : public QMainWindow {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(MainWindow)
 public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

 protected:
    void closeEvent(QCloseEvent* event) override;

 private:
    void on_cancel() noexcept;
    // Refresh (plan v1.29.0 D2): the manual re-scan — the bottom-row button
    // between Configure and Close. A silent no-op while a transaction is
    // in flight (m_running — the worker's post-transaction auto-refresh is
    // authoritative) or the Configure clone flow owns the shared progress
    // dialog (m_future_watcher); otherwise init_kernels() (the verbatim
    // shared refresh), purge_stale_rows(), and the idempotent
    // build_kernel_info_header() re-extraction — all on the main thread.
    void on_refresh() noexcept;
    // The Refresh flow's stale-row purge step (plan v1.29.0 D4): removes
    // the rebuilt tree rows whose kernel is neither a real repo/AUR
    // package (has_pkg) nor installed (is_installed) — the greyed-out
    // built/folder residue. View-level only: m_kernels, the curated list,
    // /etc/pacman.conf, and the local DB are untouched; the "Install from
    // directory…" pseudo-row always survives.
    void purge_stale_rows() noexcept;
    void on_execute() noexcept;
    void on_schedext_config() noexcept;
    void on_configure() noexcept;
    // D6 (plan v1.24.0): the bottom-left build-dir picker — "Browse…" opens
    // a folder dialog over the current build directory; an accepted choice
    // is persisted via utils::set_build_dir (QSettings, no cache) and the
    // path label is refreshed immediately. The label (buildDirLabel) is the
    // confirmation — quiet persistence, no second dialog. update_build_dir_label()
    // mirrors the current utils::build_repo_path() into the label text +
    // tooltip; it runs once at construction (the label never shows empty at
    // runtime) and after each accepted browse.
    void on_browse_build_dir() noexcept;
    void update_build_dir_label() noexcept;
    // D1 (plan v1.24.0): the "Install from directory…" pseudo-row flow —
    // folder picker (QFileDialog) -> install_kernel::install_from_directory
    // (the C2 planner, real escalated runner) -> boot-instructions dialog
    // -> same-thread init_kernels() refresh. Reached only from the
    // dedicated one-action context-menu branch of on_kernel_context_menu.
    void on_install_from_directory() noexcept;
    void on_kernel_context_menu(const QPoint& pos) noexcept;
    // The nvidia driver gate's single Qt glue (chunk 3, plan D1/D3/D7):
    // evaluates the driver-packaging verdict for one install target on
    // the main thread BEFORE any install flow runs and acts on it —
    // PROCEED silently; WARN_MIGRATE offers the one-click migration
    // (blocking, real rc, post-migration verify); ENSURE_HEADERS informs
    // that the headers ride along in the install; WARN_ONLY warns +
    // banners. Wired to trigger A (on_execute), B (the context-menu
    // install branch) and C (on_install_from_directory). Returns false
    // only when the user aborts the install.
    bool run_driver_gate(const driver_gate::GateTarget& target);
    // D7: show the persistent status-bar banner (the exact verdict text
    // — the module's; this method only owns the label). Shown on a
    // declined fix, hidden on a successful migration.
    void show_driver_banner(const QString& text);
    // D4 (plan v1.30.0): show/hide the pacmanLockBanner per the
    // file-local pacman_lock_held() probe. The 2 s poll
    // (m_pacman_lock_timer) drives it; hidden by default; the ctor's
    // one initial call pins the state at startup. Warning only —
    // Execute is never disabled.
    void update_pacman_lock_banner() noexcept;

    void check_uncheck_item() noexcept;

    void item_changed(QTreeWidgetItem* item, int column) noexcept;

    void init_kernels() noexcept;

    std::atomic_bool m_running;
    std::atomic_bool m_thread_running{true};
    std::mutex m_mutex;
    std::condition_variable m_cv;

    QStringList m_change_list;

    QProgressDialog* m_conf_progress_dialog{nullptr};
    QProgressBar* m_conf_progress_bar{nullptr};
    QFutureWatcher<void> m_future_watcher;

    // The D7 persistent status-bar banner (chunk 3): a permanent,
    // hidden-by-default label next to the version label; shown on a
    // driver-gate decline, hidden on a successful migration.
    QLabel* m_driver_banner = nullptr;

    // D4 (plan v1.30.0): the dynamic pacman-lock banner poll — the
    // 2 s QTimer that re-runs the file-local pacman_lock_held() probe
    // and shows/hides the hidden-by-default pacmanLockBanner (the
    // ctor's one initial call pins the state at startup). Value
    // member, parented to the window (stops with it). Warning only:
    // it never disables Execute.
    QTimer m_pacman_lock_timer;

    // The shared resize-time ellipsis filter (plan v1.30.0 D5, the
    // file-local ElideFilter above): window-parented (created in the
    // ctor, so it outlives the per-build header labels — the idempotent
    // rebuild re-installs fresh labels, never a stale filter), installed
    // on the build-dir path label (once) and on each header value label
    // as it is created; it elides the "km_full_text" property to the
    // current width on resize (short values stay byte-identical).
    ElideFilter* m_elide_filter = nullptr;

    // The "Active Kernel Information" header (chunk 2, plan v1.28.0 D4;
    // re-extractable on Refresh, plan v1.29.0 D3): the uic-created QFrame
    // (m_ui->kernelInfoHeader) — a direct child of the central
    // verticalLayout (the first layout item; the v1.30.0 scroll-area band
    // was removed with the section-title reflow). Aliased here so the
    // builder (build_kernel_info_header) and the Refresh path (on_refresh)
    // address the same widget directly. The null state is the frame's
    // "not yet built" flag the builder's idempotent teardown preamble keys
    // on: the member is set on the first (one-shot ctor) call, and a
    // non-null state on a later call marks a rebuild (teardown +
    // byte-identical re-render).
    QFrame* m_kernel_info_header = nullptr;

    QThread* m_worker_th = new QThread(this);
    Work* m_worker{nullptr};

    alpm_errno_t m_err{};
    alpm_handle_t* m_handle                   = utils::parse_alpm(utils::alpm_root, utils::alpm_libdir, &m_err);
    std::vector<Kernel> m_kernels             = Kernel::get_kernels(m_handle);
    std::unique_ptr<Ui::MainWindow> m_ui      = std::make_unique<Ui::MainWindow>();
    std::unique_ptr<ConfWindow> m_conf_window = std::make_unique<ConfWindow>();
#ifdef WITH_SCX_MANAGER
    // Optional scx-manager support (WU-5): nullable on purpose. Instantiated in
    // the ctor only when scx-manager is compiled in, so generic builds never
    // require the scxctl::SchedExtWindow constructor to succeed.
    std::unique_ptr<scxctl::SchedExtWindow> m_sched_window{nullptr};
#endif

    void build_change_list(QTreeWidgetItem* item) noexcept;
    void set_progress_dialog() noexcept;
    // The "Active Kernel Information" header builder (chunk 2, plan
    // v1.28.0 D4; idempotent, plan v1.29.0 D3; the color hierarchy,
    // plan v1.30.0 D2; the hero line + 4x3 grid reflow, this cycle):
    // one kernel_info::extract_kernel_info() per call (the module
    // caches its expensive work per file, so a repeat call is a fast
    // re-read, not a re-scan), then a QVBoxLayout on
    // m_ui->kernelInfoHeader — the hero line (Release in the +2 pt
    // bold value font, Compiler + Arch in the regular one) + a
    // 4-column x 3-row grid of the remaining 12 key/value pairs =
    // 30 labels (15 keys + 15 values) with deterministic objectNames,
    // neutral keys (the theme's primary text), a light-gray value
    // base (the palette Mid), the desaturated-sage green and the
    // degraded amber per the file-local info_color rule, and the
    // system fixed font — over the elevated card stylesheet (the
    // gray-alpha overlay + 1 px border + 8 px radius). No main
    // title, no section titles, no scroll area. Read-only: no
    // signals, no interactive widgets.
    // Idempotent: a repeat call (on_refresh — the second caller; the
    // one-shot ctor call is the initial build) first tears down the
    // previous layout tree + labels (the uic-owned frame survives)
    // and re-renders them byte-identically (30 → 30, no duplication).
    void build_kernel_info_header() noexcept;
};

#endif  // MAINWINDOW_HPP_
