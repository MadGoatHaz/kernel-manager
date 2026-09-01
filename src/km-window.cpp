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

// NOLINTBEGIN(bugprone-unhandled-exception-at-new)

#include "km-window.hpp"
#include "conf-window.hpp"
#include "install_kernel.hpp"
#include "kernel.hpp"
#include "known_kernels.hpp"
#include "utils.hpp"

#include <algorithm>   // for any_of, find_if
#include <filesystem>  // for exists
#include <future>
#include <ranges>       // for ranges::*
#include <span>         // for span
#include <string_view>  // for string_view
#include <thread>       // for this_thread

#include <fmt/core.h>

#include <QCoreApplication>
#include <QDialog>
#include <QFutureWatcher>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QTimer>
#include <QTreeWidgetItem>
#include <QtConcurrent/QtConcurrent>

namespace fs = std::filesystem;

namespace {
bool install_packages(alpm_handle_t* handle, const std::span<Kernel>& kernels, const std::span<std::string>& selected_list) {
    for (const auto& selected : selected_list) {
        const auto& kernel = std::ranges::find_if(kernels, [selected](auto&& el) { return el.get_raw() == selected; });
        if ((kernel != kernels.end()) && (!kernel->is_installed() || kernel->is_update_available())) {
            if (!kernel->install()) {
                fmt::print(stderr, "failed to add package to be installed ({})\n", alpm_strerror(alpm_errno(handle)));
            }
        }
    }
    return true;
}

bool remove_packages(alpm_handle_t* handle, const std::span<Kernel>& kernels, const std::span<std::string>& selected_list) {
    for (const auto& selected : selected_list) {
        const auto& kernel = std::ranges::find_if(kernels, [selected](auto&& el) { return el.get_raw() == selected; });
        if ((kernel != kernels.end()) && (kernel->is_installed())) {
            if (!kernel->remove()) {
                fmt::print(stderr, "failed to add package to be removed ({})\n", alpm_strerror(alpm_errno(handle)));
            }
        }
    }

    return true;
}

bool is_kernels_change_state(alpm_handle_t* handle, std::span<std::string> kernel_install_list, std::span<std::string> kernel_removal_list) {
    if (handle == nullptr) {
        return false;
    }
    auto* local_db = alpm_get_localdb(handle);

    if (std::ranges::any_of(kernel_install_list, [local_db](auto&& kernel_install) { return nullptr != alpm_db_get_pkg(local_db, kernel_install.data()); })) {
        return true;
    }
    if (std::ranges::any_of(kernel_removal_list, [local_db](auto&& kernel_removal) { return nullptr == alpm_db_get_pkg(local_db, kernel_removal.data()); })) {
        return true;
    }

    return false;
}

// E14: whether the named repo section is registered among the handle's sync
// DBs (scans alpm_get_syncdbs by name, same list-walk as
// alpm_utils.cpp:is_package_in_sync_db). A section absent from
// /etc/pacman.conf is simply not found (=> false).
bool is_repo_in_syncdbs(alpm_handle_t* handle, std::string_view repo) {
    if (handle == nullptr) {
        return false;
    }
    for (alpm_list_t* i = alpm_get_syncdbs(handle); i != nullptr; i = i->next) {
        auto* db = reinterpret_cast<alpm_db_t*>(i->data);
        const char* db_name = alpm_db_get_name(db);
        if (db_name == nullptr) {
            continue;
        }
        if (repo == db_name) {
            return true;
        }
    }
    return false;
}

// E14: live availability of a kernel's pre-compiled package: the table flags
// AND the package actually present where it's documented (a pacman repo in an
// enabled sync DB, or the AUR; the AUR probe degrades to false without the
// AUR tooling).
bool is_precompiled_available_live(const std::string& name) {
    if (auto e = km::find_kernel(name)) {
        const KnownKernel* k = *e;
        return k->precompiled_available && !k->install_package.empty()
            && utils::is_package_available(k->install_package, k->install_repo);
    }
    return false;
}

void init_kernels_tree_widget(QTreeWidget* tree_kernels, std::span<Kernel> kernels, alpm_handle_t* handle) noexcept {
    for (auto& kernel : kernels) {
        auto* widget_item = new KernelTreeWidgetItem(tree_kernels);
        widget_item->setCheckState(TreeCol::Check, Qt::Unchecked);
        widget_item->setText(TreeCol::PkgName, kernel.get_raw());
        // Hover tooltip on the PkgName: what this kernel is and who it's
        // for. description_for() accepts the repo-prefixed raw name and
        // always returns a non-empty string (curated entry or a synthesized
        // fallback line), so every row gets a tooltip.
        const QString base_tooltip{QString::fromStdString(km::description_for(kernel.get_raw(), kernel.category()))};
        // E14: info-rows (curated kernels absent from every enabled repo,
        // m_pkg == nullptr) gain a suffix explaining WHY they're not
        // installable right now: the repo is registered but the package is
        // not in its DB (=> `pacman -Sy`) vs. the repo is not enabled at all
        // (=> add the section to /etc/pacman.conf). Live rows keep the
        // plain base tooltip.
        // D4 (plan v1.23.0): one shared sync-DB result per row, used by both
        // the PkgName tooltip below and the Version annotation (no second
        // is_repo_in_syncdbs call per row).
        const bool repo_enabled = is_repo_in_syncdbs(handle, kernel.get_repo());
        widget_item->setToolTip(TreeCol::PkgName, kernel.has_pkg()
                ? base_tooltip
                : base_tooltip + (repo_enabled
                        ? QStringLiteral(" — not in the '%1' DB (pacman -Sy)").arg(QString::fromStdString(std::string{kernel.get_repo()}))
                        : QStringLiteral(" — repo '%1' not enabled (add it to /etc/pacman.conf)").arg(QString::fromStdString(std::string{kernel.get_repo()}))));
        // D4 (plan v1.23.0): widget-level Version annotation. An info-row
        // (m_pkg == nullptr) whose repo is absent from the handle's sync DBs
        // — i.e. not enabled — shows "— (repo not enabled)" plus a
        // Version-cell tooltip naming the repo and the right-click
        // remediation; an info-row whose repo IS enabled but whose package is
        // missing from its DB (the E14 `pacman -Sy` case) keeps the bare
        // "—". Live rows show kernel.version() byte-identical to today.
        // Data layer untouched: Kernel::version() still returns "—" for
        // info-rows (the k11 assertions hold) — this is presentation only.
        // Sort note: operator< passes the cell text through
        // alpm_pkg_vercmp, which already tolerates non-version strings like
        // the bare "—"; the annotation only appears on rows that were
        // already "—", so relative row order is unchanged and the
        // comparator stays untouched.
        widget_item->setText(TreeCol::Version, kernel.has_pkg()
                ? QString::fromStdString(kernel.version())
                : (repo_enabled ? QStringLiteral("—") : QStringLiteral("— (repo not enabled)")));
        if (!kernel.has_pkg() && !repo_enabled) {
            widget_item->setToolTip(TreeCol::Version, QStringLiteral(
                        "Version unavailable — repo '%1' is not enabled. Right-click the row to add it.")
                        .arg(QString::fromStdString(std::string{kernel.get_repo()})));
        }
        widget_item->setText(TreeCol::Category, QString::fromStdString(std::string{kernel.category()}));
        // E14: installed-on-system indicator (D4): "✓" when the package is
        // in the alpm local DB (kernel.is_installed(), name-based lookup —
        // valid for live and info rows alike), "—" otherwise. Availability
        // (whether it CAN be installed right now) is the context menu's
        // concern, not this column's.
        // Row flags: display-only text cells (no ItemIsEditable — the
        // indicator, and the other text columns, are read-only); the Choose
        // checkbox (ItemIsUserCheckable) stays live for installable rows but
        // is dropped for info-rows (m_pkg == nullptr) whose Choose cell must
        // be disabled — they cannot be selected for install/remove.
        widget_item->setText(TreeCol::Install, kernel.is_installed() ? QStringLiteral("✓") : QStringLiteral("—"));
        widget_item->setFlags(kernel.has_pkg()
                ? Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable
                : Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        // D3 (plan v1.23.0): an info-row (m_pkg == nullptr) Choose cell gets a
        // non-interactive lock QLabel instead of the confusing disabled
        // checkbox. Glyph resolution: theme icon "dialog-lock-symbolic", then
        // "locked", then a centered "🔒" text fallback (a non-null icon is
        // rendered at 16x16). The tooltip names the state + the remediation and
        // reuses C1's shared repo_enabled (one sync-DB result per row): repo
        // enabled but the package missing from its DB (=> `pacman -Sy`) vs.
        // repo not enabled at all (=> right-click to add it — C5 wires that
        // action). Live rows keep the native checkbox (no setItemWidget); the
        // flags above already drop ItemIsUserCheckable on info-rows, so no
        // checkbox renders behind the label. build_change_list /
        // item_changed / check_uncheck_item are untouched: an info-row cannot
        // be toggled, so no new signal paths.
        if (!kernel.has_pkg()) {
            auto* lock = new QLabel(tree_kernels);
            QIcon icon = QIcon::fromTheme(QStringLiteral("dialog-lock-symbolic"));
            if (icon.isNull()) {
                icon = QIcon::fromTheme(QStringLiteral("locked"));
            }
            if (icon.isNull()) {
                lock->setText(QStringLiteral("🔒"));
                lock->setAlignment(Qt::AlignCenter);
            } else {
                lock->setPixmap(icon.pixmap(16, 16));
            }
            lock->setToolTip((repo_enabled
                    ? QStringLiteral("Not in the '%1' DB — run `pacman -Sy`")
                    : QStringLiteral("Repo '%1' not enabled — right-click the row to add it"))
                    .arg(QString::fromStdString(std::string{kernel.get_repo()})));
            tree_kernels->setItemWidget(widget_item, TreeCol::Check, lock);
        }
        if (kernel.is_installed()) {
            const std::string_view kernel_installed_db = kernel.get_installed_db();
            if (!kernel_installed_db.empty() && kernel_installed_db != kernel.get_repo()) {
                continue;
            }
            widget_item->setText(TreeCol::Immutable, QStringLiteral("true"));
            widget_item->setCheckState(TreeCol::Check, Qt::Checked);
        }
    }
}

// Show the ordered boot-selection steps (boot_instructions_for output) in a
// small read-only dialog: one numbered step per line, text is selectable
// so the user can copy it.
void show_boot_instructions(QWidget* parent, const std::vector<std::string>& instructions) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Boot instructions"));

    auto* layout = new QVBoxLayout(&dialog);

    auto* text = new QTextEdit(&dialog);
    text->setReadOnly(true);
    QStringList lines;
    for (std::size_t i = 0; i < instructions.size(); ++i) {
        lines << QStringLiteral("%1. %2").arg(static_cast<int>(i + 1)).arg(QString::fromStdString(instructions[i]));
    }
    text->setPlainText(lines.join(QLatin1Char('\n')));
    layout->addWidget(text);

    auto* close_button = new QPushButton(QObject::tr("Close"), &dialog);
    layout->addWidget(close_button, 0, Qt::AlignRight);
    QObject::connect(close_button, &QPushButton::clicked, &dialog, &QDialog::accept);

    dialog.exec();
}
}  // namespace

MainWindow::MainWindow(QWidget* parent)
  : QMainWindow(parent) {
    m_ui->setupUi(this);
    setWindowIcon(QApplication::windowIcon());  // explicit dedicated icon; the .ui no longer overrides; robust to Qt app-fallback semantics

    setAttribute(Qt::WA_NativeWindow);
    setWindowFlags(Qt::Window);  // for the close, min and max buttons

    // Create worker thread
    m_worker = new Work([&]() {
        while (m_thread_running.load(std::memory_order_consume)) {
            std::unique_lock<std::mutex> lock(m_mutex);
            fmt::print(stderr, "Waiting... \n");

            m_cv.wait(lock, [&] { return m_running.load(std::memory_order_consume); });

            if (m_running.load(std::memory_order_consume) && m_thread_running.load(std::memory_order_consume)) {
                m_ui->ok->setEnabled(false);

                std::vector<std::string> change_list(static_cast<std::size_t>(m_change_list.size()));
                for (int i = 0; i < m_change_list.size(); ++i) {
                    change_list[static_cast<std::size_t>(i)] = m_change_list[i].toStdString();
                }

                install_packages(m_handle, m_kernels, change_list);
                remove_packages(m_handle, m_kernels, change_list);
                Kernel::commit_transaction();

                // check if we need to re-init kernels
                // [1.1]
                auto& kernel_install_list = Kernel::get_install_list();
                auto& kernel_removal_list = Kernel::get_removal_list();

                // NOTE: we don't want to override handle, because we would need to invalidate kernels then.
                auto* temp_handle = utils::parse_alpm(utils::alpm_root, utils::alpm_libdir, &m_err);
                if (temp_handle == nullptr) {
                    QMessageBox::critical(this, tr("Kernel Manager"), tr("Failed to initialize alpm handle (%1)").arg(alpm_strerror(m_err)));
                }

                // [1.2]
                // iterate over install and removal lists and check if any of the packages
                // in the lists were either installed or removed
                const bool is_kernel_status_changed = is_kernels_change_state(temp_handle, std::span{kernel_install_list}, std::span{kernel_removal_list});

                // [1.3]
                // if kernel status has changed, then re-init alpm handler,
                // fetch kernels and repopulate tree widget again
                if (is_kernel_status_changed) {
                    if (m_handle != nullptr && utils::release_alpm(m_handle, &m_err) != 0) {
                        QMessageBox::critical(this, tr("Kernel Manager"), tr("Failed to release alpm handle (%1)").arg(alpm_strerror(m_err)));
                    }

                    m_handle = temp_handle;
                    m_kernels.clear();
                    m_kernels = Kernel::get_kernels(m_handle);

                    // schedule init_kernels to be executed in the main thread
                    QMetaObject::invokeMethod(this, "init_kernels", Qt::QueuedConnection);
                }

                // clear install and removal lists
                kernel_install_list.clear();
                kernel_removal_list.clear();
                m_change_list.clear();

                m_running.store(false, std::memory_order_relaxed);
                m_ui->ok->setEnabled(!is_kernel_status_changed);
            }
        }
    });

    m_worker->moveToThread(m_worker_th);
    // name to appear in ps, task manager, etc.
    m_worker_th->setObjectName("WorkerThread");

    m_ui->ok->setEnabled(false);

    // Instantiate the sched-ext config window only when scx-manager support is
    // compiled in (WU-5); m_sched_window stays nullptr otherwise.
#ifdef WITH_SCX_MANAGER
    m_sched_window = std::make_unique<scxctl::SchedExtWindow>();
#endif

    // Hide sched-ext button in case we are not on kernel with sched-ext, or
    // scx-manager support is not compiled in at all (WU-5).
#ifdef WITH_SCX_MANAGER
    if (!fs::exists("/sys/kernel/sched_ext/state")) {
        m_ui->schedext->setHidden(true);
    }
#else
    m_ui->schedext->setHidden(true);
#endif

    // Setup progress dialog
    set_progress_dialog();

    // Setup configure window
    connect(&m_future_watcher, &QFutureWatcher<void>::finished, this, [&]() {
        m_conf_progress_dialog->hide();
        if (m_future_watcher.future().isCanceled()) {
            return;
        }
        if (m_future_watcher.future().isFinished()) {
            m_conf_window->show();
            return;
        }
        QMessageBox::critical(this, tr("Kernel Manager"), tr("Failed to clone repository!\nPlease check your internet connection and try again"));
    });
    connect(m_conf_progress_dialog, &QProgressDialog::canceled, this, [&]() {
        fmt::print("the operation was canceled!\n");
        // that doesn't really stop execution, it just hides the progress dialog
        m_future_watcher.cancel();
    });

    // Setup tree widget
    auto* tree_kernels = m_ui->treeKernels;
    // The Install indicator column (K10) stays visible; only the internal
    // Immutable status column is hidden.
    tree_kernels->hideColumn(TreeCol::Immutable);  // Immutable status true/false
    tree_kernels->header()->setSectionResizeMode(QHeaderView::ResizeToContents);

    // Set context menu policy and wire the per-kernel actions
    // (install pre-compiled / build custom / show boot instructions).
    tree_kernels->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_kernels, &QTreeWidget::customContextMenuRequested, this, &MainWindow::on_kernel_context_menu);

    tree_kernels->blockSignals(true);

    // TODO(vnepogodin): parallelize it
    auto a2 = std::async(std::launch::deferred, [&] {
        const std::lock_guard<std::mutex> guard(m_mutex);
        init_kernels_tree_widget(tree_kernels, std::span{m_kernels}, m_handle);
    });

    if (m_kernels.empty()) {
        QMessageBox::critical(this, tr("Kernel Manager"), tr("No kernels found!\nPlease run `pacman -Sy` to update DB!\nThis is needed for the app to work properly"));
    }

    // Connect buttons signal
    connect(m_ui->cancel, &QPushButton::clicked, this, &MainWindow::on_cancel);
    connect(m_ui->ok, &QPushButton::clicked, this, &MainWindow::on_execute);
    connect(m_ui->configure, &QPushButton::clicked, this, &MainWindow::on_configure);
#ifdef WITH_SCX_MANAGER
    if (m_sched_window != nullptr) {
        connect(m_ui->schedext, &QPushButton::clicked, this, &MainWindow::on_schedext_config);
    }
#endif

    // Connect worker thread signals
    connect(m_worker_th, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker_th, &QThread::started, m_worker, &Work::doHeavyCalculations, Qt::QueuedConnection);

    // check/uncheck tree items space-bar press or double-click
    auto* shortcutToggle = new QShortcut(Qt::Key_Space, this);
    connect(shortcutToggle, &QShortcut::activated, this, &MainWindow::check_uncheck_item);

    // Connect tree widget
    connect(tree_kernels, &QTreeWidget::itemChanged, this, &MainWindow::item_changed);
    connect(tree_kernels, &QTreeWidget::itemDoubleClicked, [tree_kernels](QTreeWidgetItem* item) { tree_kernels->setCurrentItem(item); });
    connect(tree_kernels, &QTreeWidget::itemDoubleClicked, this, &MainWindow::check_uncheck_item);

    // Wait for async function to finish
    a2.wait();
    tree_kernels->blockSignals(false);
}

MainWindow::~MainWindow() {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (m_worker_th != nullptr) {
        m_worker_th->exit();
    }
}

// Setup progress dialog
void MainWindow::set_progress_dialog() noexcept {
    m_conf_progress_dialog = new QProgressDialog(this);
    m_conf_progress_bar    = new QProgressBar(m_conf_progress_dialog);

    // Set progress dialog
    m_conf_progress_bar->setMinimum(0);
    m_conf_progress_bar->setMaximum(0);
    m_conf_progress_dialog->setMinimum(0);
    m_conf_progress_dialog->setMaximum(0);

    // Set progress dialog properties
    m_conf_progress_dialog->setWindowModality(Qt::WindowModal);
    m_conf_progress_dialog->setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint
        | Qt::WindowSystemMenuHint | Qt::WindowStaysOnTopHint);
    m_conf_progress_dialog->setLabelText(tr("Please wait...\nWe are preparing configuration window for you\ncloning PKGBUILDs.."));
    m_conf_progress_dialog->setAutoClose(false);
    m_conf_progress_dialog->setBar(m_conf_progress_bar);
    m_conf_progress_bar->setTextVisible(false);
    m_conf_progress_dialog->reset();
}

void MainWindow::check_uncheck_item() noexcept {
    if (auto* t_widget = qobject_cast<QTreeWidget*>(focusWidget())) {
        if (t_widget->currentItem() == nullptr || t_widget->currentItem()->childCount() > 0) {
            return;
        }
        auto new_state = (t_widget->currentItem()->checkState(TreeCol::Check) == Qt::Checked) ? Qt::Unchecked : Qt::Checked;
        t_widget->currentItem()->setCheckState(TreeCol::Check, new_state);
    }
}

// When selecting on item in the list
void MainWindow::item_changed(QTreeWidgetItem* item, int /*unused*/) noexcept {
    if (item->checkState(TreeCol::Check) == Qt::Checked) {
        m_ui->treeKernels->setCurrentItem(item);
    }
    build_change_list(item);
}

// Build the change_list when selecting on item in the tree
void MainWindow::build_change_list(QTreeWidgetItem* item) noexcept {
    auto item_text = item->text(TreeCol::PkgName);
    auto immutable = item->text(TreeCol::Immutable);
    if (immutable == "true" && item->checkState(0) == Qt::Unchecked) {
        m_ui->ok->setEnabled(true);
        m_change_list.append(item_text);
        return;
    }

    if (immutable == "true" && item->checkState(0) == Qt::Checked) {
        m_change_list.removeOne(item_text);
    } else if (item->checkState(0) == Qt::Checked) {
        m_ui->ok->setEnabled(true);
        m_change_list.append(item_text);
    } else {
        m_change_list.removeOne(item_text);
    }

    if (m_change_list.isEmpty()) {
        m_ui->ok->setEnabled(false);
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Exit worker thread
    m_running.store(true, std::memory_order_relaxed);
    m_thread_running.store(false, std::memory_order_relaxed);
    m_cv.notify_all();

    // Release libalpm handle
    alpm_release(m_handle);

    // Execute parent function
    QWidget::closeEvent(event);
}

void MainWindow::on_configure() noexcept {
    // show progress dialog to indicate user something is happening
    m_conf_progress_dialog->setLabelText(tr("Please wait...\nWe are preparing configuration window for you\ncloning PKGBUILDs.."));
    m_conf_progress_dialog->show();

    // Auto-populate the build source for the kernel selected in the tree
    // (data-driven kernel -> source mapping); a repeat for the same kernel
    // is a no-op, so a manual source choice is preserved.
    if (auto* current = m_ui->treeKernels->currentItem()) {
        m_conf_window->apply_source_for_kernel(current->text(TreeCol::PkgName).toStdString());
    }

    // Apply the user-selected build source before the background prepare.
    m_conf_window->sync_build_source();

    // NOTE: the future created by QtConcurrent::run is not cancelable.
    // prepare in the background, without blocking the UI
    m_future_watcher.setFuture(QtConcurrent::run([this] {
        utils::prepare_build_environment();
        m_conf_window->refresh_flavors();
        m_conf_window->reset_patches_data_tab();
    }));
}

// Right-click on a kernel row: offer the per-kernel actions "Install
// pre-compiled <name>" (enabled only when the curated table has a
// pre-compiled path for it — build-only kernels like linux-tkg get it
// disabled), "Add repo '<repo>'" (D2, conditional: curated pacman-repo
// kernels whose repo is not enabled in /etc/pacman.conf), "Build custom
// <name>" (the existing Configure flow), and "Show boot instructions"
// (the post-install selection steps for the detected bootloader,
// K5+K6+K8).
void MainWindow::on_kernel_context_menu(const QPoint& pos) noexcept {
    auto* tree_kernels = m_ui->treeKernels;

    // The row under the cursor (customContextMenuRequested positions are
    // viewport coordinates); fall back to the current row.
    QTreeWidgetItem* item = tree_kernels->itemAt(pos);
    if (item == nullptr) {
        item = tree_kernels->currentItem();
    }
    if (item == nullptr) {
        return;
    }
    tree_kernels->setCurrentItem(item);

    // The tree PkgName is "repo/name"; the table and the install lookups
    // key on the bare kernel name (prefix stripped by the shared helper —
    // the same one the row tooltip uses).
    const QString pkg_raw = item->text(TreeCol::PkgName);
    const std::string name{km::kernel_name_from_raw(pkg_raw.toStdString())};
    const QString display = QString::fromStdString(name);

    QMenu menu(this);

    auto* install_action = menu.addAction(tr("Install pre-compiled %1").arg(display));
    // E14: the action is only offered when the kernel has a documented
    // pre-compiled path AND the package is actually available where it's
    // documented (an enabled sync DB / the AUR) — never promising a pacman
    // run that would fail at the missing/disabled repo.
    install_action->setEnabled(km::is_installable(name) && is_precompiled_available_live(name));

    // D2 (plan v1.23.0): "Add repo '<repo>'" — offered only for a curated
    // pacman-repo kernel whose repo is not enabled in /etc/pacman.conf:
    // the repo name is table-derived (never user text; the AUR is excluded
    // via classify_repo) and is_repo_enabled reads the live config (not the
    // alpm handle, which predates the add). Already-enabled repos get no
    // action. The click flow (confirm → add_repo_to_pacman_conf → list
    // refresh) is handled after the install block below.
    QAction* add_repo_action = nullptr;
    std::string repo_to_add{};
    if (auto e = km::find_kernel(name)) {
        const KnownKernel* k = *e;
        if (utils::classify_repo(k->install_repo) == utils::PackageSource::PACMAN_REPO && !utils::is_repo_enabled(k->install_repo)) {
            repo_to_add = k->install_repo;
            add_repo_action = menu.addAction(tr("Add repo '%1'").arg(QString::fromStdString(repo_to_add)));
        }
    }

    auto* build_action = menu.addAction(tr("Build custom %1").arg(display));

    auto* boot_action = menu.addAction(tr("Show boot instructions"));

    const QAction* chosen = menu.exec(tree_kernels->viewport()->mapToGlobal(pos));
    if (chosen == nullptr) {
        return;
    }

    if (chosen == install_action) {
        QMessageBox confirm(this);
        confirm.setText(tr("Install the pre-compiled package for '%1'?").arg(display));
        confirm.setInformativeText(tr("This installs the package as root (pacman) and refreshes the initramfs."));
        confirm.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        confirm.setDefaultButton(QMessageBox::No);
        if (confirm.exec() != QMessageBox::Yes) {
            return;
        }

        // The K8 name-based API: plan from the table, then execute the
        // steps through the existing pkexec terminal path. Synchronous
        // for now (a progress dialog is a later refinement); the
        // outcome is reported below either way.
        const auto plan = plan_install(name);
        std::string error;
        if (!execute_plan(plan, error)) {
            QMessageBox::critical(this, tr("Kernel Manager"), tr("Failed to install '%1':\n%2").arg(display, QString::fromStdString(error)));
            return;
        }

        QString message = tr("Installed '%1'. Select it at the next boot:").arg(display);
        const auto instructions = boot_instructions_for(name);
        for (std::size_t i = 0; i < instructions.size(); ++i) {
            message += QLatin1Char('\n');
            message += QStringLiteral("%1. %2").arg(static_cast<int>(i + 1)).arg(QString::fromStdString(instructions[i]));
        }
        QMessageBox::information(this, tr("Kernel Manager"), message);
        return;
    }

    if (chosen == add_repo_action) {
        // D2 (plan v1.23.0): confirm first (default No), then enable the
        // repo through the existing pkexec terminal layer —
        // utils::add_repo_to_pacman_conf runs
        // "$KM_HELPER_DIR/repo_add.sh '<repo>'" as an administrator
        // (backup + append + `pacman -Sy`, C4+C6); the synchronous
        // terminal blocks until the user presses enter, so the operation
        // is complete before we report back.
        const QString repo = QString::fromStdString(repo_to_add);
        QMessageBox confirm(this);
        confirm.setText(tr("Enable the '%1' pacman repository?").arg(repo));
        confirm.setInformativeText(tr("This appends the [%1] section to /etc/pacman.conf (a backup is written first) and runs `pacman -Sy` as an administrator.").arg(repo));
        confirm.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        confirm.setDefaultButton(QMessageBox::No);
        if (confirm.exec() != QMessageBox::Yes) {
            return;
        }
        const int rc = utils::add_repo_to_pacman_conf(repo_to_add);
        // C5: the terminal-helper/pkexec chain's exit code is unreliable
        // (it can be non-zero even when the script succeeded), so the
        // outcome is judged by the ACTUAL STATE — whether the repo
        // section is now enabled in /etc/pacman.conf — not by rc. rc is
        // kept only for the failure message as a diagnostic.
        const bool repo_now_enabled = utils::is_repo_enabled(repo_to_add);
        if (repo_now_enabled) {
            // Success — the repo is enabled regardless of the terminal
            // exit code.
            QMessageBox::information(this, tr("Repository enabled"),
                tr("Repository '%1' enabled — refreshing the kernel list…").arg(repo));
            // Same-thread slot: re-parse the alpm handle + re-fetch +
            // rebuild so the just-enabled repo's rows go live immediately
            // (D2).
            init_kernels();
        } else {
            // Genuine failure — the repo is NOT enabled in
            // /etc/pacman.conf.
            QMessageBox::critical(this, tr("Failed"),
                tr("Failed to enable repository '%1' (exit code %2). Check the terminal output.")
                    .arg(repo).arg(rc));
        }
        return;
    }

    if (chosen == build_action) {
        // The existing Configure flow: on_configure auto-populates the
        // build source from the current tree row (set above), prepares
        // the build environment in the background, then shows the window.
        on_configure();
        return;
    }

    if (chosen == boot_action) {
        show_boot_instructions(this, boot_instructions_for(name));
    }
}

void MainWindow::on_cancel() noexcept {
    close();
}

void Work::doHeavyCalculations() {
    m_func();
}

void MainWindow::init_kernels() noexcept {
    // show progress dialog to indicate user something is happening
    m_conf_progress_dialog->setLabelText(tr("Please wait...\nInitializing kernels.."));
    m_conf_progress_dialog->show();

    // D2 (plan v1.23.0): re-establish the sync-DB view before the rebuild —
    // a repo enabled after app start (this slot's Add-repo success path, or
    // the worker's post-transaction re-init) is only visible through a
    // fresh alpm handle, so release + re-parse it and re-fetch the kernel
    // list under m_mutex (serializes with the worker's transaction swap;
    // the worker's own pre-swap parse stays untouched, the double parse is
    // harmless).
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        if (m_handle != nullptr) {
            utils::release_alpm(m_handle, &m_err);
        }
        m_handle = utils::parse_alpm(utils::alpm_root, utils::alpm_libdir, &m_err);
    }
    if (m_handle == nullptr) {
        QMessageBox::critical(this, tr("Kernel Manager"), tr("Failed to initialize alpm handle (%1)").arg(alpm_strerror(m_err)));
        m_conf_progress_dialog->hide();
        return;
    }
    {
        const std::lock_guard<std::mutex> guard(m_mutex);
        m_kernels = Kernel::get_kernels(m_handle);
    }

    auto* tree_kernels = m_ui->treeKernels;
    tree_kernels->blockSignals(true);
    tree_kernels->clear();

    // NOTE: I don't think this should be parallelized, because it's already not running on the main thread
    init_kernels_tree_widget(tree_kernels, std::span{m_kernels}, m_handle);

    tree_kernels->blockSignals(false);
    m_conf_progress_dialog->hide();
}

void MainWindow::on_execute() noexcept {
    if (m_running.load(std::memory_order_consume)) {
        return;
    }
    m_running.store(true, std::memory_order_relaxed);
    m_thread_running.store(true, std::memory_order_relaxed);
    m_cv.notify_all();
    m_worker_th->start();
}

void MainWindow::on_schedext_config() noexcept {
#ifdef WITH_SCX_MANAGER
    if (m_sched_window == nullptr) {
        QMessageBox::warning(this, tr("Kernel Manager"), tr("scx-manager is not installed."));
        return;
    }
    m_sched_window->show();
#else
    // scx-manager support is not compiled in (WU-5): the button is hidden in
    // the ctor, this path is defensive only.
    QMessageBox::warning(this, tr("Kernel Manager"), tr("scx-manager is not installed."));
#endif
}

bool KernelTreeWidgetItem::operator<(const QTreeWidgetItem& other) const {
    const auto sort_col = treeWidget()->sortColumn();
    if (sort_col != TreeCol::Version) {
        return QTreeWidgetItem::operator<(other);
    }

    auto get_comparable_version = [](QString&& version_string) {
        using namespace std::string_view_literals;
        auto std_str = std::move(version_string).toStdString();
        for (auto&& prefix : {"∨"sv, "∧"sv}) {
            if (std_str.starts_with(prefix)) {
                return std_str.substr(prefix.size());
            }
        }
        return std_str;
    };

    const auto& version_a = get_comparable_version(text(sort_col));
    const auto& version_b = get_comparable_version(other.text(sort_col));

    return alpm_pkg_vercmp(version_a.c_str(), version_b.c_str()) < 0;
}

// NOLINTEND(bugprone-unhandled-exception-at-new)
