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

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wfloat-conversion"
#pragma clang diagnostic ignored "-Wdouble-promotion"
#pragma clang diagnostic ignored "-Wimplicit-int-float-conversion"
#pragma clang diagnostic ignored "-Wdeprecated-enum-enum-conversion"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#elif defined(__GNUC__)
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
#include "kernel.hpp"
#include "utils.hpp"

#ifdef WITH_SCX_MANAGER
#include <scx-manager/schedext-window.hpp>
#endif

#include <array>
#include <condition_variable>
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

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

class Work final : public QObject {
    Q_OBJECT

 public:
    using function_t = std::function<void()>;
    explicit Work(function_t&& func)
      : m_func(std::move(func)) { }
    ~Work() = default;

 public:
    void doHeavyCalculations();

 private:
    function_t m_func;
};

namespace TreeCol {
enum { Check,
    PkgName,
    Version,
    Category,
    Install,   // K10: installed-on-system indicator ("✓" / "—") from the alpm local DB, read-only; availability is the context-menu's concern
    Immutable };
}

class KernelTreeWidgetItem : public QTreeWidgetItem {
 public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem& other) const override;
};

class MainWindow final : public QMainWindow {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(MainWindow)
 public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

 protected:
    void closeEvent(QCloseEvent* event) override;

 private:
    void on_cancel() noexcept;
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

    void check_uncheck_item() noexcept;

    void item_changed(QTreeWidgetItem* item, int column) noexcept;

    void init_kernels() noexcept;

    std::atomic_bool m_running{};
    std::atomic_bool m_thread_running{true};
    std::mutex m_mutex{};
    std::condition_variable m_cv{};

    QStringList m_change_list{};

    QProgressDialog* m_conf_progress_dialog{nullptr};
    QProgressBar* m_conf_progress_bar{nullptr};
    QFutureWatcher<void> m_future_watcher{};

    QThread* m_worker_th = new QThread(this);
    Work* m_worker{nullptr};

    alpm_errno_t m_err{};
    alpm_handle_t* m_handle                                = utils::parse_alpm(utils::alpm_root, utils::alpm_libdir, &m_err);
    std::vector<Kernel> m_kernels                          = Kernel::get_kernels(m_handle);
    std::unique_ptr<Ui::MainWindow> m_ui                   = std::make_unique<Ui::MainWindow>();
    std::unique_ptr<ConfWindow> m_conf_window              = std::make_unique<ConfWindow>();
#ifdef WITH_SCX_MANAGER
    // Optional scx-manager support (WU-5): nullable on purpose. Instantiated in
    // the ctor only when scx-manager is compiled in, so generic builds never
    // require the scxctl::SchedExtWindow constructor to succeed.
    std::unique_ptr<scxctl::SchedExtWindow> m_sched_window{nullptr};
#endif

    void build_change_list(QTreeWidgetItem* item) noexcept;
    void set_progress_dialog() noexcept;
};

#endif  // MAINWINDOW_HPP_
