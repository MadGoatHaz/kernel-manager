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
#include <QFileDialog>
#include <QFutureWatcher>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QStatusBar>
#include <QTextEdit>
#include <QTimer>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

namespace {
// D1 (plan v1.24.0): the single identity constant for the "Install from
// directory…" pseudo-row — file-scope (visible to init_kernels_tree_widget
// and MainWindow::on_kernel_context_menu alike). It is a UTF-8 string
// literal (NOT QStringLiteral): tr() takes const char*, so the row text and
// both recognition comparisons tr() the SAME constant, keeping the row and
// its menu/locale handling consistent in a translated build (the spec's
// QStringLiteral form is not tr()-able; the constant's value is identical).
// The k12 harness mirrors this literal.
const auto kDirectoryRowRaw = "Install from directory…";

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
    for (const alpm_list_t* i = alpm_get_syncdbs(handle); i != nullptr; i = i->next) {
        auto* db            = reinterpret_cast<alpm_db_t*>(i->data);
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
        widget_item->setCheckState(static_cast<int>(TreeCol::Check), Qt::Unchecked);
        widget_item->setText(static_cast<int>(TreeCol::PkgName), kernel.get_raw());
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
        widget_item->setToolTip(static_cast<int>(TreeCol::PkgName), kernel.has_pkg() ? base_tooltip : base_tooltip + (repo_enabled ? QStringLiteral(" — not in the '%1' DB (pacman -Sy)").arg(QString::fromStdString(std::string{kernel.get_repo()})) : QStringLiteral(" — repo '%1' not enabled (add it to /etc/pacman.conf)").arg(QString::fromStdString(std::string{kernel.get_repo()}))));
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
        // The Version cell: the real version for a live row; for an
        // info-row, the bare "—" when its repo is enabled but the package
        // is not in its DB (the E14 `pacman -Sy` case), and the annotated
        // "— (repo not enabled)" when the repo itself is not enabled.
        QString version_cell;
        if (kernel.has_pkg()) {
            version_cell = QString::fromStdString(kernel.version());
        } else if (repo_enabled) {
            version_cell = QStringLiteral("—");
        } else {
            version_cell = QStringLiteral("— (repo not enabled)");
        }
        widget_item->setText(static_cast<int>(TreeCol::Version), version_cell);
        if (!kernel.has_pkg() && !repo_enabled) {
            widget_item->setToolTip(static_cast<int>(TreeCol::Version), QStringLiteral("Version unavailable — repo '%1' is not enabled. Right-click the row to add it.").arg(QString::fromStdString(std::string{kernel.get_repo()})));
        }
        widget_item->setText(static_cast<int>(TreeCol::Category), QString::fromStdString(std::string{kernel.category()}));
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
        widget_item->setText(static_cast<int>(TreeCol::Install), kernel.is_installed() ? QStringLiteral("✓") : QStringLiteral("—"));
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
            tree_kernels->setItemWidget(widget_item, static_cast<int>(TreeCol::Check), lock);
        }
        if (kernel.is_installed()) {
            const std::string_view kernel_installed_db = kernel.get_installed_db();
            if (!kernel_installed_db.empty() && kernel_installed_db != kernel.get_repo()) {
                continue;
            }
            widget_item->setText(static_cast<int>(TreeCol::Immutable), QStringLiteral("true"));
            widget_item->setCheckState(static_cast<int>(TreeCol::Check), Qt::Checked);
        }
    }

    // D1 (plan v1.24.0): the "Install from directory…" pseudo-row — appended
    // AFTER the kernel loop so BOTH rebuild paths render it (the ctor's
    // deferred init above and the init_kernels() slot re-run this function),
    // and it survives every list refresh. It is non-interactive on purpose:
    // the worker thread cannot open a QFileDialog (cross-thread GUI = UB)
    // and only sees m_kernels + the change-list strings, so a checkbox-driven
    // row would need worker special cases; without ItemIsUserCheckable there
    // is no checkbox, no itemChanged/build_change_list signal path, and the
    // row is invisible to the worker. Right-click is the only entry point
    // (the dedicated one-action branch in on_kernel_context_menu).
    auto* dir_item = new KernelTreeWidgetItem(tree_kernels);
    dir_item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);  // no ItemIsUserCheckable (D1)
    // QObject::tr (public static — this is a free function, not a
    // MainWindow member): the single identity constant, locale-consistent
    // with the on_kernel_context_menu comparison below.
    dir_item->setText(static_cast<int>(TreeCol::PkgName), QObject::tr(kDirectoryRowRaw));
    dir_item->setText(static_cast<int>(TreeCol::Version), QStringLiteral("—"));
    dir_item->setText(static_cast<int>(TreeCol::Category), QStringLiteral("Local"));
    dir_item->setText(static_cast<int>(TreeCol::Install), QStringLiteral("—"));
    // One shared tooltip text (the D1 string, QStringLiteral per the
    // deferred-lupdate precedent) on both the PkgName cell and the Check
    // cell: what the row is + the right-click remediation.
    const auto dir_tooltip = QStringLiteral(
        "Install a locally built kernel package from a folder: right-click to choose the folder containing the built *.pkg.tar.zst package(s).");
    dir_item->setToolTip(static_cast<int>(TreeCol::PkgName), dir_tooltip);
    // The Check cell: the D3 lock-label recipe with the glyph swapped —
    // a non-interactive QLabel showing a folder-open icon (theme
    // "folder-open" → "folder" → centered "📁" text fallback, 16x16 when a
    // theme icon resolves; offscreen has no icon theme, so the emoji is the
    // common case there).
    auto* dir_check = new QLabel(tree_kernels);
    auto dir_icon   = QIcon::fromTheme(QStringLiteral("folder-open"));
    if (dir_icon.isNull()) {
        dir_icon = QIcon::fromTheme(QStringLiteral("folder"));
    }
    if (dir_icon.isNull()) {
        dir_check->setText(QStringLiteral("📁"));
        dir_check->setAlignment(Qt::AlignCenter);
    } else {
        dir_check->setPixmap(dir_icon.pixmap(16, 16));
    }
    dir_check->setToolTip(dir_tooltip);
    tree_kernels->setItemWidget(dir_item, static_cast<int>(TreeCol::Check), dir_check);
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
        lines << QStringLiteral("%1. %2").arg(static_cast<int>(i + 1)).arg(QString::fromStdString(instructions.at(i)));
    }
    text->setPlainText(lines.join(QLatin1Char('\n')));
    layout->addWidget(text);

    auto* close_button = new QPushButton(QObject::tr("Close"), &dialog);
    layout->addWidget(close_button, 0, Qt::AlignRight);
    QObject::connect(close_button, &QPushButton::clicked, &dialog, &QDialog::accept);

    dialog.exec();
}

// ── The 2-state install outcome dialog (simplify-K1/K2) ───────────────
// The install result's InstallVerdict (simplify-K1) is one of two
// states, each shown as a DISTINCT dialog. The terminal's output is the
// source of truth for the post-install details (nvidia DKMS, initramfs,
// BLS entry — done by the distro's ALPM hooks inside the pacman
// transaction), so the app only reports success or failure:
//   INSTALL_SUCCESS → green (Information): the install command exited 0
//                     — the package is in; reboot when ready.
//   INSTALL_FAILED  → red (Critical): the install command exited
//                     non-zero — the text carries the real rc and points
//                     at the terminal output for the details.
// verdict_dialog() builds the (icon, text) for a given verdict + rc. It
// is pure and testable: the driver injects each verdict and checks the
// text + icon without driving a blocking QMessageBox. The caller raises
// + activates the window BEFORE the modal (H1) and shows the
// QMessageBox with the returned icon + text.

// The data the outcome dialog is built from: the verdict + the install
// command's real exit code (`rc`; -1 = no install command ran, e.g. the
// build-only graceful failure — the reason is in the result's `error`).
struct VerdictDialogSpec {
    InstallVerdict verdict = InstallVerdict::INSTALL_FAILED;
    int rc                 = -1;
};

// The dialog a verdict produces: the icon (green/red) + the full text.
struct VerdictDialog {
    QMessageBox::Icon icon = QMessageBox::Critical;
    QString text;
};

[[nodiscard]] VerdictDialog verdict_dialog(const VerdictDialogSpec& spec) {
    VerdictDialog d{};
    if (spec.verdict == InstallVerdict::INSTALL_SUCCESS) {
        d.icon = QMessageBox::Information;
        d.text = QObject::tr("Kernel installed successfully. You can reboot when ready.");
    } else {
        // INSTALL_FAILED: the install command exited non-zero (or never
        // ran — rc -1); the terminal output is the detail record.
        d.icon = QMessageBox::Critical;
        d.text = QObject::tr("Kernel installation failed (rc=%1). Check the terminal output for details.").arg(spec.rc);
    }
    return d;
}

// ── The nvidia driver gate (chunk 3, plan D1/D3/D7) ───────────────────
// The gate is a main-thread pre-flight BEFORE any install flow: the
// Qt-free driver_gate module evaluates the driver-packaging state and
// the single MainWindow::run_driver_gate acts on the verdict with
// plain-language dialogs. The helpers below are its Qt-side details (the
// H1 window-raise pattern, the trigger-A union target parsing, the
// migration command + verify).

// The H1 pattern (the conf-window.cpp handle_build_done precedent): bring
// the window to the front + fire a desktop-notification backup BEFORE a
// modal — after a long operation the window can sit buried behind other
// windows, and a modal parented to a buried window is invisible to the
// user. The gate's modals all share it (notify-send is fire-and-forget,
// like the terminal-helper's own notifications).
void bring_window_forward(QWidget* window) {
    window->raise();
    window->activateWindow();
    QProcess::startDetached("notify-send",
        {"--app-name=Kernel Manager",
            "nvidia driver check",
            "Kernel Manager has a question about the nvidia driver before this install."});
}

// The version() display markers ("∨" = the installed version is newer,
// "∧" = an update is available — kernel.cpp version()) are UI-only
// decoration: strip a leading marker so the raw version is what the
// gate's build-dir check uses (the same prefix strip operator< uses for
// sorting).
[[gnu::pure]] [[nodiscard]] std::string strip_display_marker(const std::string& version) {
    using namespace std::string_view_literals;
    for (const auto prefix : {"∨"sv, "∧"sv}) {
        if (version.starts_with(prefix)) {
            return version.substr(prefix.size());
        }
    }
    return version;
}

// The target's kernel names (the trigger-A union carries them space-
// joined in GateTarget.kernel; triggers B/C carry one name or none):
// split + drop the empties.
[[gnu::pure]] [[nodiscard]] std::vector<std::string> target_names(const driver_gate::GateTarget& target) {
    std::vector<std::string> names{};
    const std::string_view kernel{target.kernel};
    std::size_t pos = 0;
    while (pos < kernel.size()) {
        const std::size_t end       = kernel.find(' ', pos);
        const std::string_view name = (end == std::string_view::npos) ? kernel.substr(pos)
                                                                      : kernel.substr(pos, end - pos);
        if (!name.empty()) {
            names.emplace_back(name);
        }
        if (end == std::string_view::npos) {
            break;
        }
        pos = end + 1;
    }
    return names;
}

// The WARN_MIGRATE command to run (D3): the DKMS package + the union of
// each target name's derived -headers package. A single name (triggers
// B/C) rebuilds EXACTLY the verdict's migration_cmd; the trigger-A union
// extends it to every selected kernel's headers (plan D1 row A — one
// migration command for all of them, --needed keeps it idempotent).
[[gnu::pure]] [[nodiscard]] std::string migration_command(const driver_gate::GateTarget& target, const std::string& dkms_package) {
    std::string cmd = "pacman -S --needed --asexplicit " + dkms_package;
    for (const auto& name : target_names(target)) {
        const auto headers = driver_gate::derive_headers_pkg(name);
        if (!headers.empty()) {
            cmd += " " + headers;
        }
    }
    return cmd;
}

// The D3 post-migration verify's headers clause, per target name (a
// single name is exactly the verdict's headers_package): true iff every
// name's derived -headers package is in the local DB (no names = nothing
// to check). The local-DB fact is authoritative (the dkms tool's own
// state is best-effort and never counts against success).
[[nodiscard]] bool migration_headers_installed(const driver_gate::GateTarget& target) {
    for (const auto& name : target_names(target)) {
        const auto headers = driver_gate::derive_headers_pkg(name);
        if (!headers.empty() && !driver_gate::package_installed(headers)) {
            return false;
        }
    }
    return true;
}
}  // namespace

MainWindow::MainWindow(QWidget* parent)
  : QMainWindow(parent), m_driver_banner(new QLabel(parent)) {
    m_ui->setupUi(this);
    setWindowIcon(QApplication::windowIcon());  // explicit dedicated icon; the .ui no longer overrides; robust to Qt app-fallback semantics

    // Version display (v1.25.0 follow-up): the PROJECT_VERSION from CMake
    // (exposed as APP_VERSION) is shown in the window title and as a
    // permanent status-bar label, so the running build is easy to identify.
    setWindowTitle(tr("Kernel Manager %1").arg(APP_VERSION));
    statusBar()->addPermanentWidget(new QLabel(tr("v%1").arg(APP_VERSION)));

    // The D7 persistent banner (chunk 3): a permanent, hidden-by-default
    // status-bar label next to the version label (created in the member
    // initializer list above). It shows the gate's banner text when the
    // user declines the nvidia driver fix (the custom kernel comes up
    // without GPU acceleration) and hides on a successful migration; it
    // lives for the session only — the gate re-evaluates fresh on every
    // install attempt (no QSettings persistence, the next run's verdict
    // is the source of truth).
    m_driver_banner->hide();
    statusBar()->addPermanentWidget(m_driver_banner);

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
                    change_list.at(static_cast<std::size_t>(i)) = m_change_list.at(i).toStdString();
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
    if (!std::filesystem::exists("/sys/kernel/sched_ext/state")) {
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
    tree_kernels->hideColumn(static_cast<int>(TreeCol::Immutable));  // Immutable status true/false
    tree_kernels->header()->setSectionResizeMode(QHeaderView::ResizeToContents);

    // Set context menu policy and wire the per-kernel actions
    // (install pre-compiled / build custom / show boot instructions).
    tree_kernels->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_kernels, &QTreeWidget::customContextMenuRequested, this, &MainWindow::on_kernel_context_menu);

    tree_kernels->blockSignals(true);

    // TODO(vnepogodin): parallelize it
    auto a2 = std::async(std::launch::deferred, [&] {
        const std::scoped_lock guard(m_mutex);
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
    // D6 (plan v1.24.0): the "Browse…" button — a folder picker over the
    // current build directory whose accepted choice is persisted via
    // utils::set_build_dir (no cache) and immediately reflected in the path
    // label below. The label is refreshed once here (after setupUi, so it
    // never shows empty at runtime) and after each accepted browse.
    connect(m_ui->browse, &QPushButton::clicked, this, &MainWindow::on_browse_build_dir);
    update_build_dir_label();

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

    // Cycle-7 D1: the directory pseudo-row has no checkbox (v1.24.0 D1 design),
    // so build_change_list never enables the Execute button for it.
    // Selecting the row IS the enable signal; the lambda only ever sets true —
    // build_change_list retains authority to disable on an empty change list.
    connect(m_ui->treeKernels, &QTreeWidget::currentItemChanged, this,
        [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
            if (current != nullptr && current->text(static_cast<int>(TreeCol::PkgName)) == tr(kDirectoryRowRaw)) {
                m_ui->ok->setEnabled(true);
            }
        });

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
        auto new_state = (t_widget->currentItem()->checkState(static_cast<int>(TreeCol::Check)) == Qt::Checked) ? Qt::Unchecked : Qt::Checked;
        t_widget->currentItem()->setCheckState(static_cast<int>(TreeCol::Check), new_state);
    }
}

// When selecting on item in the list
void MainWindow::item_changed(QTreeWidgetItem* item, int /*unused*/) noexcept {
    if (item->checkState(static_cast<int>(TreeCol::Check)) == Qt::Checked) {
        m_ui->treeKernels->setCurrentItem(item);
    }
    build_change_list(item);
}

// Build the change_list when selecting on item in the tree
void MainWindow::build_change_list(QTreeWidgetItem* item) noexcept {
    auto item_text = item->text(static_cast<int>(TreeCol::PkgName));
    auto immutable = item->text(static_cast<int>(TreeCol::Immutable));
    if (immutable == "true" && item->checkState(0) == Qt::Unchecked) {
        m_ui->ok->setEnabled(true);
        m_change_list.append(item_text);
        return;
    }

    // Checked rows: an immutable one is unselected (removed from the
    // change list), a regular one is selected; unchecked rows (the
    // immutable+unchecked case early-returned above) are always dropped.
    if (item->checkState(0) == Qt::Checked) {
        if (immutable == "true") {
            m_change_list.removeOne(item_text);
        } else {
            m_ui->ok->setEnabled(true);
            m_change_list.append(item_text);
        }
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
        // D1 (plan v1.24.0): the "Install from directory…" pseudo-row has
        // no build source — configure is a no-op for it. The progress
        // dialog was already shown above, so hide it first (no leftover
        // spinner), then return before apply_source_for_kernel would
        // prefill a nonsense source from the row text.
        if (current->text(static_cast<int>(TreeCol::PkgName)) == tr(kDirectoryRowRaw)) {
            m_conf_progress_dialog->hide();
            return;
        }
        m_conf_window->apply_source_for_kernel(current->text(static_cast<int>(TreeCol::PkgName)).toStdString());
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

    // D1 (plan v1.24.0): the "Install from directory…" pseudo-row gets a
    // dedicated one-action menu — the generic action build below never runs
    // for it (no km::find_kernel("…") fallback oddities). Selecting the
    // action opens the folder picker + install flow (the slot below).
    if (item->text(static_cast<int>(TreeCol::PkgName)) == tr(kDirectoryRowRaw)) {
        QMenu dmenu(this);
        auto* dir_action = dmenu.addAction(tr("Install from directory…"));
        if (dmenu.exec(tree_kernels->viewport()->mapToGlobal(pos)) == dir_action) {
            on_install_from_directory();
        }
        return;
    }

    // The tree PkgName is "repo/name"; the table and the install lookups
    // key on the bare kernel name (prefix stripped by the shared helper —
    // the same one the row tooltip uses).
    const QString pkg_raw = item->text(static_cast<int>(TreeCol::PkgName));
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
    const QAction* add_repo_action = nullptr;
    std::string repo_to_add{};
    if (auto e = km::find_kernel(name)) {
        const KnownKernel* k = *e;
        if (utils::classify_repo(k->install_repo) == utils::PackageSource::PACMAN_REPO && !utils::is_repo_enabled(k->install_repo)) {
            repo_to_add     = k->install_repo;
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

        // The driver gate (chunk 3, trigger B): the curated kernel's
        // target — the kver is unknown in this context (the K8 flow has
        // no alpm handle), so the gate degrades to the headers-package
        // decision (plan D3). A declined migration or an aborted install
        // stops here, before the install runs.
        driver_gate::GateTarget target{};
        target.kernel = name;
        if (!run_driver_gate(target)) {
            return;
        }

        // The K8 install path (chunk E): build the table entry (the same
        // K1-fallback shape plan_install uses) and run the install through
        // the shared pkexec terminal path. Synchronous for now (a progress
        // dialog is a later refinement); the 2-state InstallVerdict in
        // the result decides which outcome dialog is shown below (green
        // for INSTALL_SUCCESS / red for INSTALL_FAILED).
        KnownKernel kernel{};
        if (const auto entry = km::find_kernel(name); entry.has_value()) {
            kernel = **entry;
        } else {
            kernel.name                  = name;
            kernel.default_source        = name;
            kernel.install_package       = name;
            kernel.install_repo          = "";
            kernel.precompiled_available = false;
            kernel.buildable             = true;
        }
        const InstallKernelResult result = install_kernel(kernel);

        VerdictDialogSpec spec{};
        spec.verdict = result.verdict;
        spec.rc      = result.rc;
        const auto d = verdict_dialog(spec);

        // Raise + activate BEFORE the modal (H1): after the synchronous
        // install the main window can sit buried behind the install's
        // terminal windows, and a modal parented to a buried window is
        // invisible to the user.
        raise();
        activateWindow();
        QMessageBox box(this);
        box.setIcon(d.icon);
        box.setWindowTitle(tr("Kernel Manager"));
        box.setText(d.text);
        box.exec();
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
                    .arg(repo)
                    .arg(rc));
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

// D1 (plan v1.24.0): the "Install from directory…" flow — a folder picker
// (the current build directory is the natural default start point, that's
// where locally built packages live), the C2 install_from_directory()
// with the REAL runner (the escalated pkexec terminal — the whole install
// is visible to the user, the synchronous pattern of the pre-compiled
// install above), then the 2-state outcome dialog (green for
// INSTALL_SUCCESS / red for INSTALL_FAILED) and a same-thread list
// refresh so the just-installed kernel's row appears immediately.
void MainWindow::on_install_from_directory() noexcept {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose build directory…"), QString::fromStdString(utils::build_repo_path().string()));
    if (dir.isEmpty()) {
        return;  // cancel = silent (no dialog, no refresh — D6-style)
    }

    // The driver gate (chunk 3, trigger C): the target's identity is the
    // first built package whose .PKGINFO parses to a non-headers kernel
    // name (the install_from_directory identity logic — a dir may hold
    // the kernel + its headers pair); the parsed version doubles as the
    // kver for the build-dir check. No parseable package leaves the
    // target empty (the gate degrades to the empty-target row).
    driver_gate::GateTarget target{};
    for (const auto& pkg : list_local_packages(dir.toStdString())) {
        std::string pkg_name{};
        std::string pkg_version{};
        if (read_pkginfo(pkg, pkg_name, pkg_version) && !pkg_name.ends_with("-headers")) {
            target.kernel = pkg_name;
            target.kver   = pkg_version;
            break;
        }
    }
    if (!run_driver_gate(target)) {
        return;
    }

    // The real runner: an empty CommandRunner selects run_real_command
    // (utils::runCmdTerminal, escalated). The terminal's output is the
    // source of truth for the install's details (the distro's ALPM hooks
    // do the post-install work inside the transaction); the run is also
    // logged to r.log_path as a post-mortem record.
    const DirInstallResult r = install_from_directory(dir.toStdString());

    // The outcome dialog from the verdict (simplify-K1/K2): green for
    // INSTALL_SUCCESS, red for INSTALL_FAILED (the real rc is in the
    // text; the terminal output is the detail record).
    VerdictDialogSpec spec{};
    spec.verdict = r.verdict;
    spec.rc      = r.rc;
    const auto d = verdict_dialog(spec);

    // Raise + activate BEFORE the modal (H1): after the synchronous
    // install the main window can sit buried behind the install's terminal
    // windows, and a modal parented to a buried window is invisible to the
    // user.
    raise();
    activateWindow();
    QMessageBox box(this);
    box.setIcon(d.icon);
    box.setWindowTitle(tr("Kernel Manager"));
    box.setText(d.text);
    box.exec();

    // Same-thread refresh (the C5 Add-repo pattern) so the just-installed
    // kernel's row — local/… (C1) or its repo row — appears with the
    // Installed ✓. The package is present only for INSTALL_SUCCESS;
    // INSTALL_FAILED leaves the list unchanged.
    if (r.verdict != InstallVerdict::INSTALL_FAILED) {
        init_kernels();
    }
}

// The nvidia driver gate's single Qt glue (chunk 3, plan D1/D3/D7):
// evaluate the verdict for one install target (the main-thread pre-flight
// — the trigger A/B/C call sites) and act on it BEFORE any install flow
// runs:
//   PROCEED        -> silently (no nvidia hardware, no nvidia driver, or
//                     the DKMS driver + headers are already in place).
//   WARN_MIGRATE   -> the plain-language warning + the one-click
//                     migration: the command runs in the escalated
//                     terminal (blocking — the sentinel protocol returns
//                     its real rc) and the outcome is verified against
//                     the local DB + build dir (the D3 post-migration
//                     verify); declining the migration shows the
//                     persistent D7 banner and proceeds.
//   ENSURE_HEADERS -> the informational note that the headers ride along
//                     in the install (trigger A structurally, B and C via
//                     the install engine's pairing step).
//   WARN_ONLY      -> the warning (no fix exists for this kernel):
//                     proceed shows the banner, cancel aborts.
// Returns false only when the user aborts the install (every other path
// proceeds — the banner documents the consequence of a declined fix).
bool MainWindow::run_driver_gate(const driver_gate::GateTarget& target) {
    const auto v = driver_gate::evaluate_gate(target);

    switch (v.action) {
    case driver_gate::GateAction::PROCEED:
        return true;

    case driver_gate::GateAction::ENSURE_HEADERS:
        bring_window_forward(this);
        QMessageBox::information(this, tr("Kernel Manager"), QString::fromStdString(v.message));
        return true;

    case driver_gate::GateAction::WARN_ONLY: {
        bring_window_forward(this);
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(tr("Kernel Manager"));
        box.setText(QString::fromStdString(v.message));
        auto* proceed = box.addButton(tr("Proceed"), QMessageBox::AcceptRole);
        box.addButton(tr("Cancel"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == proceed) {
            show_driver_banner(QString::fromStdString(v.banner));
            return true;
        }
        return false;
    }

    case driver_gate::GateAction::WARN_MIGRATE: {
        bring_window_forward(this);
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(tr("Kernel Manager"));
        box.setText(QString::fromStdString(v.message));
        box.addButton(tr("Migrate to %1").arg(QString::fromStdString(v.dkms_package)), QMessageBox::AcceptRole);
        auto* without = box.addButton(tr("Proceed without"), QMessageBox::ActionRole);
        auto* cancel  = box.addButton(tr("Abort install"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == cancel) {
            return false;  // the install is aborted
        }
        if (box.clickedButton() == without) {
            // Proceed without the migration: the banner documents the
            // consequence (no GPU acceleration on the custom kernel) and
            // re-shows on every later decline.
            show_driver_banner(QString::fromStdString(v.banner));
            return true;
        }

        // The one-click migration: the escalated terminal runs the
        // command and the sentinel protocol returns its real rc
        // (blocking — the user watches it happen).
        const int rc = utils::runCmdTerminal(QString::fromStdString(migration_command(target, v.dkms_package)), /*escalate=*/true);

        // The D3 post-migration verify: the real rc + the DKMS driver
        // installed + every target name's headers in the local DB + (for
        // a known kver: its build dir, or the headers still in a sync
        // repo as the soft fallback).
        std::string first_headers{};
        if (const auto names = target_names(target); !names.empty()) {
            first_headers = driver_gate::derive_headers_pkg(names.front());
        }
        const bool verified = (rc == 0)
            && driver_gate::dkms_driver_installed()
            && migration_headers_installed(target)
            && (target.kver.empty() || driver_gate::build_dir_exists(target.kver) || (!first_headers.empty() && driver_gate::package_in_sync_db(first_headers)));

        bring_window_forward(this);
        if (verified) {
            // A successful migration clears the warning.
            if (m_driver_banner != nullptr) {
                m_driver_banner->hide();
            }
            QMessageBox::information(this, tr("Kernel Manager"), tr("The DKMS driver migration succeeded. The install will continue."));
            return true;
        }
        QMessageBox critical_box(this);
        critical_box.setIcon(QMessageBox::Critical);
        critical_box.setWindowTitle(tr("Kernel Manager"));
        critical_box.setText(tr("The driver migration failed (rc=%1).").arg(rc));
        critical_box.setInformativeText(tr("Check the terminal output for details."));
        critical_box.exec();
        const auto res = QMessageBox::question(this, tr("Kernel Manager"), tr("Continue with the install anyway?"));
        if (res == QMessageBox::Yes) {
            // The migration did not verify: the banner says why the
            // custom kernel may lack GPU acceleration.
            show_driver_banner(QString::fromStdString(v.banner));
            return true;
        }
        return false;
    }
    }
    return true;  // defensive: an unknown action never blocks the install
}

// D7: show the persistent status-bar banner (the exact verdict text —
// the module's; this method only owns the label). Shown on a declined
// fix (WARN_MIGRATE "proceed without" / WARN_ONLY "proceed"), hidden on
// a successful migration.
void MainWindow::show_driver_banner(const QString& text) {
    if (m_driver_banner == nullptr) {
        return;
    }
    m_driver_banner->setText(text);
    m_driver_banner->show();
}

// D6 (plan v1.24.0): the "Browse…" flow — a folder picker whose default
// start point is the current build directory (that's where the clones and
// locally built packages live). An accepted choice is persisted via
// utils::set_build_dir (the QSettings `buildDir` key — no cache, so
// build_repo_path()/build_app_path()/aur_pkgbuilds_path() all reflect it
// immediately) and the path label is refreshed below. Cancel is a silent
// no-op: the label updating IS the feedback, and QSettings sync is
// best-effort, so there is nothing to confirm or error about.
void MainWindow::on_browse_build_dir() noexcept {
    // QString::fromStdString wraps the accessor result: Qt6's QString has
    // no implicit constructor from std::string (the QAnyStringView route
    // needs three user-defined conversions — ill-formed).
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose build directory"), QString::fromStdString(utils::build_repo_path().string()));
    if (dir.isEmpty()) {
        return;
    }
    utils::set_build_dir(dir.toStdString());
    update_build_dir_label();
}

// D6 (plan v1.24.0): refresh the bottom-left path label from the current
// build directory — the text AND the tooltip carry the full path (the
// tooltip is the guaranteed-full view for long user-picked paths). Called
// from the constructor (the label never shows empty at runtime) and after
// each accepted browse.
void MainWindow::update_build_dir_label() noexcept {
    const auto path = utils::build_repo_path().string();
    m_ui->buildDirLabel->setText(QString::fromStdString(path));
    m_ui->buildDirLabel->setToolTip(QString::fromStdString(path));
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
        const std::scoped_lock guard(m_mutex);
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
        const std::scoped_lock guard(m_mutex);
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
    // Cycle-7 D1: if the selected row is the "Install from directory…" pseudo-row,
    // run the directory flow directly (the working right-click path) instead of
    // starting the worker thread (which would see an empty change list and no-op).
    // A checked kernel + selected pseudo-row ⇒ the directory flow wins (documented);
    // the checked kernel stays in m_change_list for a later Execute.
    if (auto* current = m_ui->treeKernels->currentItem()) {
        if (current->text(static_cast<int>(TreeCol::PkgName)) == tr(kDirectoryRowRaw)) {
            on_install_from_directory();
            if (m_change_list.isEmpty()) {
                m_ui->ok->setEnabled(false);
            }
            return;
        }
    }

    if (m_running.load(std::memory_order_consume)) {
        return;
    }

    // The driver gate (chunk 3, trigger A): a main-thread pre-flight
    // BEFORE the worker starts (a modal dialog + a possible system-state
    // change never run on the worker thread). The target is the union of
    // the selected INSTALLABLE rows — the mirror of the worker's
    // install_packages predicate (row found && has_pkg() && not
    // installed or an update is available): the kernel names merge
    // space-joined (their union is the migration's headers list) and the
    // kver is the first non-"" one (its display marker stripped). No
    // installable row (e.g. removals only) ⇒ nothing to gate. A declined
    // or aborted gate stops the install before it starts.
    driver_gate::GateTarget target{};
    bool has_installable = false;
    for (const auto& selected : m_change_list) {
        auto kernel = std::ranges::find_if(m_kernels, [selected](auto&& el) { return el.get_raw() == selected.toStdString(); });
        if ((kernel != m_kernels.end()) && kernel->has_pkg() && (!kernel->is_installed() || kernel->is_update_available())) {
            const std::string name{km::kernel_name_from_raw(kernel->get_raw())};
            if (target.kernel.empty()) {
                target.kernel = name;
            } else {
                target.kernel += " " + name;
            }
            if (target.kver.empty()) {
                target.kver = strip_display_marker(kernel->version());
            }
            has_installable = true;
        }
    }
    if (has_installable && !run_driver_gate(target)) {
        return;  // the user aborted the install
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
    if (sort_col != static_cast<int>(TreeCol::Version)) {
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
