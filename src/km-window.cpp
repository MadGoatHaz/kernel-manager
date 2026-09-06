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
#include "kernel_info.hpp"
#include "known_kernels.hpp"
#include "utils.hpp"

#include <algorithm>   // for any_of, find_if, max
#include <cctype>      // for tolower
#include <filesystem>  // for exists
#include <future>
#include <ranges>       // for ranges::*
#include <span>         // for span
#include <string_view>  // for string_view
#include <thread>       // for this_thread

#include <fmt/core.h>

#include <QColor>
#include <QCoreApplication>
#include <QDialog>
#include <QEvent>
#include <QFileDialog>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPalette>
#include <QProcess>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
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

// ── The Active Kernel Information header (chunk 2, plan v1.28.0 D4) ─────
// The header's value-label color rule: file-local, pure, and TOTAL — every
// possible kernel_info display value yields exactly one color (the rule is
// over the value, not the field: each field's value set is classified by
// its meaning). Gray = empty (unknown / not available) or a feature
// explicitly off (disabled / none / never / off — the module's display
// strings vary in case, so the comparison is case-insensitive). Yellow =
// working but degraded: a non-native CPU target (generic / custom), lazy or
// voluntary preemption, -O2, thin LTO. Green = every other non-empty fact
// (an active/present value — native, a family target, x86-64-vN, full LTO,
// -O3, 1000 Hz, enabled, dynamic/full preemption, active MGLRU,
// madvise/always THP, bbr, tsc, …).
enum class InfoColor : std::uint8_t { Green,
    Yellow,
    Gray };

// Lowercased copy (the color rule compares case-insensitively — the
// kernel_info lower() precedent).
[[gnu::pure]] [[nodiscard]] std::string to_lower(std::string_view value) {
    std::string out{};
    out.reserve(value.size());
    for (const char c : value) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

[[gnu::pure]] [[nodiscard]] InfoColor info_color(const std::string& value) {
    const std::string v = to_lower(value);
    if (v.empty() || v == "disabled" || v == "none" || v == "never" || v == "off") {
        return InfoColor::Gray;
    }
    if (v == "generic" || v == "custom" || v == "lazy" || v == "voluntary" || v == "o2" || v == "-o2" || v == "thin lto") {
        return InfoColor::Yellow;
    }
    return InfoColor::Green;
}

}  // namespace

// D5 (plan v1.30.0): the resize-time ellipsis for the long-string labels —
// the header's 15 value labels (installed in add_row) and the build-dir
// path label (installed in the ctor). File-local (the definition exists in
// this one TU only; the hpp carries the matching forward declaration the
// raw-pointer member keys on — global scope, NOT the anonymous namespace:
// a namespaced definition would be a distinct class the hpp declaration
// cannot name). It keys on the "km_full_text" dynamic property (the
// unelided value — the tooltip source), elides it right-truncated to the
// label's current width on Resize + Show, and re-sets the text only when
// it differs (a short value that fits renders byte-identical — the elide
// is a no-op fit check, so a label that never needs truncation stays
// exactly as the builder left it). Every other event type returns false
// untouched; the filter is passive (it never consumes an event) and
// outlives the per-build labels (window-parented; the idempotent rebuild
// re-installs fresh labels, the old ones are deleted with the frame's
// children — no dangling).
class ElideFilter final : public QObject {
 public:
    using QObject::QObject;
    bool eventFilter(QObject* obj, QEvent* event) override;
};

bool ElideFilter::eventFilter(QObject* obj, QEvent* event) {
    if (event->type() != QEvent::Resize && event->type() != QEvent::Show) {
        return false;  // every other event type is untouched
    }
    const auto full_prop = obj->property("km_full_text");
    if (!full_prop.isValid()) {
        return false;  // not a tracked label (no full text to elide)
    }
    auto* label = qobject_cast<QLabel*>(obj);
    if (label == nullptr) {
        return false;
    }
    const QString full   = full_prop.toString();
    const QString elided = label->fontMetrics().elidedText(full, Qt::ElideRight, qMax(0, label->width()));
    if (elided != label->text()) {
        label->setText(elided);  // a no-op fit keeps the original text
    }
    if (label->toolTip().isEmpty()) {
        label->setToolTip(full);  // the full text is always one tooltip away
    }
    return false;  // passive: the label handles the event normally after
}

MainWindow::MainWindow(QWidget* parent)
  : QMainWindow(parent), m_driver_banner(new QLabel(parent)), m_elide_filter(new ElideFilter(this)) {
    m_ui->setupUi(this);
    // D5 (plan v1.30.0): the shared resize-time ellipsis filter — created
    // once in the member-initializer-list above (window-parented, so it
    // outlives the per-build header labels) and installed here on the
    // build-dir path label (the widget exists from setupUi); the header's
    // value labels install it in build_kernel_info_header as they are
    // created.
    m_ui->buildDirLabel->installEventFilter(m_elide_filter);
    setWindowIcon(QApplication::windowIcon());  // explicit dedicated icon; the .ui no longer overrides; robust to Qt app-fallback semantics

    // Version display (v1.25.0 follow-up): the PROJECT_VERSION from CMake
    // (exposed as APP_VERSION) is shown in the window title and as a
    // permanent status-bar label, so the running build is easy to identify.
    setWindowTitle(tr("Kernel Manager %1").arg(APP_VERSION));
    statusBar()->addPermanentWidget(new QLabel(tr("v%1").arg(APP_VERSION)));

    // The "Active Kernel Information" header (chunk 2, plan v1.28.0 D4;
    // plan v1.29.0 D3): build + populate it — this ctor call is the
    // initial (one-shot) build; on_refresh is the second, idempotent
    // caller (the builder's teardown preamble keeps a repeat call from
    // duplicating the grid + labels; the module caches its expensive
    // work per file — a ≤ 8-sample disassembly budget + a 2 MiB cap —
    // so a repeat extraction is a fast re-read). The booted kernel is
    // invariant while the app runs. The frame + scroll area exist from
    // setupUi; the grid and its labels are code-built.
    build_kernel_info_header();

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
    // Refresh (plan v1.29.0 D2): the bottom-row button between Configure
    // and Close — the manual re-scan (on_refresh).
    connect(m_ui->refresh, &QPushButton::clicked, this, &MainWindow::on_refresh);
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

// The "Active Kernel Information" header (chunk 2, plan v1.28.0 D4;
// idempotent, plan v1.29.0 D3; the 3-column card reflow, plan v1.30.0
// D1): the read-only, color-coded panel at the top of the MainWindow
// showing the BOOTED kernel's parameters in 5 sections — Kernel &
// Toolchain, CPU Arch Target, Optimization & LTO, Scheduling & Latency,
// Runtime Subsystems.
// The data is kernel_info::extract_kernel_info() — the booted kernel is
// invariant while the app runs, and the module caches its expensive work
// per file (the ≤ 8-sample disassembly budget is process-wide), so a
// repeat extraction is a fast re-read, not a re-scan. Callers: the ctor
// (the initial one-shot build) and on_refresh (the second, idempotent
// caller — the D3 teardown preamble below keeps a repeat call from
// stacking a duplicate grid + labels). The uic-created QFrame
// (m_ui->kernelInfoHeader, inside the kernelInfoScroll QScrollArea — the
// first layout item) hosts a code-built QGridLayout: three equal-stretch
// columns (no per-column minimum — the 800 px frame minimum is the only
// width floor; below it the band scrolls horizontally as needed) laid
// out by the section-major 13-row placement map of plan v1.30.0 D1 —
// row 0 the kiMainTitle (span 3), then per section a span-3 title row +
// its key/value cells in rows of 3 (Release/BuildDate/Compiler,
// TargetArch/IsaLevel/IsaValidation, Lto/Optimization, TickRate/
// SchedExt/Preemption, Mglru/Thp/TcpCongestion/Clocksource) = 36 labels
// (1 + 5 + 15 keys + 15 values), each with a deterministic objectName
// (kiMainTitle, kiTitle1..5, ki<Section><Key> / …Key) so a test driver
// can address them. The card keeps its natural height: the frame's
// minimum is pinned to the grid's sizeHint (the load-bearing line at the
// builder's end — the scroll area is widgetResizable, so without it the
// 13-row card is squeezed to the 150 px band and clipped with no
// scrollbar), and the band scrolls vertically as needed (the .ui's
// AsNeeded policy). Styling per plan v1.30.0 D2: the elevated card
// (the theme-neutral gray-alpha overlay + 1 px border + 8 px radius),
// section titles bold +1 pt with a bottom border, keys in the smaller
// neutral font (the theme's primary text — secondary by size, not by
// color), and values in the system fixed font color-coded by
// info_color (desaturated-sage green active / degraded amber / the Mid
// light-gray off-or-unknown — an empty value renders "—" in the Mid
// gray). No signals,
// no interactive widgets — the header is informational only; the tree
// below keeps all interaction.
void MainWindow::build_kernel_info_header() noexcept {
    auto* frame = m_ui->kernelInfoHeader;
    if (frame == nullptr) {
        return;
    }
    // Idempotency preamble (plan v1.29.0 D3): the member is null before
    // the first build (the one-shot ctor call) and aliased to the frame
    // on it, so a non-null member here marks a repeat call (on_refresh).
    // Tear down the previous build before the unchanged build path below
    // re-renders it byte-identically:
    //   1. delete frame->layout() — the QGridLayout; the nested per-cell
    //      QHBoxLayouts are QObject children of it (probe-verified on
    //      this Qt), so they die with it and no item survives dangling.
    //   2. qDeleteAll(frame->findChildren<QWidget*>()) — the 36 labels
    //      (direct children of the frame; findChildren excludes the
    //      frame itself, so the uic-owned frame survives).
    // The order is layout-then-labels: the layout's item wrappers are
    // destroyed while their target widgets are still alive (a wrapper
    // holds a non-owning pointer and never dereferences it in its
    // destructor), and the labels are destroyed only after no layout
    // references them — no dangling item at any point.
    const bool already_built = (m_kernel_info_header != nullptr);
    m_kernel_info_header     = frame;
    if (already_built) {
        delete frame->layout();
        qDeleteAll(frame->findChildren<QWidget*>());
    }

    // The extraction (the module's probes all degrade to "" — no signal,
    // no crash — and none of them prints; a repeat call is a fast
    // re-read — the per-file cache + the process-wide sample budget).
    const kernel_info::KernelInfo info = kernel_info::extract_kernel_info();

    // The elevated card (plan v1.30.0 D2: the gray-alpha overlay raised
    // 26 → 40 so it reads as a panel, not a wash, the border alpha
    // 64 → 110 for the 1 px subtle edge, the radius 4 → 8 px for the
    // card shape; theme-neutral in light and dark, the v1.28.0 D4
    // precedent) + the 800 px minimum width (the .ui carries it too —
    // this keeps the intent visible in code).
    frame->setStyleSheet(QStringLiteral(
        "#kernelInfoHeader { background: rgba(127,127,127,40); border: 1px solid rgba(127,127,127,110); border-radius: 8px; }"));
    frame->setMinimumWidth(800);

    auto* grid = new QGridLayout(frame);
    grid->setContentsMargins(10, 8, 10, 8);
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(4);
    for (int col = 0; col < 3; ++col) {
        grid->setColumnStretch(col, 1);
    }

    // The label fonts: the keys derive from the window's base font one
    // point smaller (secondary text); the section titles +1 point bold;
    // the values use the system fixed font (a semi-monospace feel without
    // hardcoding a family). A pixel-based base font (no point size) keeps
    // its size for the keys and only gains the bold for the titles.
    const QFont base_font = font();
    const qreal base_pt   = base_font.pointSizeF();
    QFont key_font        = base_font;
    if (base_pt > 0.0) {
        key_font.setPointSizeF(base_pt - 1.0);
    }
    QFont section_font = base_font;
    section_font.setBold(true);
    if (base_pt > 0.0) {
        section_font.setPointSizeF(base_pt + 1.0);
    }
    const QFont value_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    // The color hierarchy (plan v1.30.0 D2): green + yellow are fixed
    // (no QPalette green role exists — both stay readable on light and
    // dark over the card's overlay); gray is the palette's Mid (the
    // theme-adaptive light-gray value base). Keys are neutral — the
    // frame's WindowText, the theme's primary text (set at the
    // key-palette site in add_row below; secondary by size, not by
    // color). Tiers: desaturated-sage green #5E8A6E active; the
    // degraded amber #9A7700 preserved (the brief-silence reading — it
    // stands unchanged); the Mid light gray for off-or-unknown values.
    const QColor green{0x5e, 0x8a, 0x6e};
    const QColor yellow{0x9a, 0x77, 0x00};
    const QColor gray = frame->palette().color(QPalette::Mid);

    // The main title (row 0, column-span 3, bold).
    auto* main_title = new QLabel(tr("Active Kernel Information"), frame);
    main_title->setObjectName(QStringLiteral("kiMainTitle"));
    main_title->setFont(section_font);
    grid->addWidget(main_title, 0, 0, 1, 3);

    // One key/value row in (row, col): the nested [key, value] HBox — the
    // key in the smaller neutral font (the frame's WindowText — the
    // theme's primary text; secondary by size, not color), the value in
    // the fixed font color-coded by info_color (an empty value renders
    // "—" in the Mid gray).
    const auto add_row = [&](int row, int col, const QString& key_text, const QString& value_name, const std::string& value) {
        auto* key = new QLabel(key_text, frame);
        key->setObjectName(value_name + "Key");
        key->setFont(key_font);
        QPalette key_palette = key->palette();
        key_palette.setColor(QPalette::WindowText, frame->palette().color(QPalette::WindowText));
        key->setPalette(key_palette);

        const QString shown = value.empty() ? QStringLiteral("—") : QString::fromStdString(value);
        auto* value_label   = new QLabel(shown, frame);
        value_label->setObjectName(value_name);
        value_label->setFont(value_font);
        const InfoColor color = info_color(value);
        QColor value_color    = gray;
        if (color == InfoColor::Green) {
            value_color = green;
        } else if (color == InfoColor::Yellow) {
            value_color = yellow;
        }
        QPalette value_palette = value_label->palette();
        value_palette.setColor(QPalette::WindowText, value_color);
        value_label->setPalette(value_palette);

        // D5 (plan v1.30.0): the guaranteed-full contract — the dynamic
        // property carries the unelided value (the ElideFilter's source),
        // the tooltip always shows it, and the shared filter elides the
        // displayed text on resize (a value that fits stays verbatim —
        // the no-op fit check).
        value_label->setProperty("km_full_text", shown);
        value_label->setToolTip(shown);
        value_label->installEventFilter(m_elide_filter);

        auto* cell = new QHBoxLayout();
        cell->setContentsMargins(0, 0, 0, 0);
        cell->setSpacing(6);
        cell->addWidget(key);
        cell->addWidget(value_label);
        grid->addLayout(cell, row, col);
    };

    // One section title (a dedicated row, spanning all 3 columns): bold
    // +1 pt with the subtle bottom border.
    const auto add_section_title = [&](int row, const QString& title, const QString& name) {
        auto* section_title = new QLabel(title, frame);
        section_title->setObjectName(name);
        section_title->setFont(section_font);
        section_title->setStyleSheet(QStringLiteral("border-bottom: 1px solid rgba(127,127,127,90);"));
        grid->addWidget(section_title, row, 0, 1, 3);
    };

    // 1 — Kernel & Toolchain.
    add_section_title(1, tr("Kernel & Toolchain"), QStringLiteral("kiTitle1"));
    add_row(2, 0, tr("Release"), QStringLiteral("kiKtRelease"), info.release);
    add_row(2, 1, tr("Build date"), QStringLiteral("kiKtBuildDate"), info.build_date);
    add_row(2, 2, tr("Compiler"), QStringLiteral("kiKtCompiler"), info.compiler);

    // 2 — CPU Arch Target.
    add_section_title(3, tr("CPU Arch Target"), QStringLiteral("kiTitle2"));
    add_row(4, 0, tr("Target arch"), QStringLiteral("kiCtTargetArch"), info.target_arch);
    add_row(4, 1, tr("ISA level"), QStringLiteral("kiCtIsaLevel"), info.isa_level);
    add_row(4, 2, tr("ISA validation"), QStringLiteral("kiCtIsaValidation"), info.instruction_validation);

    // 3 — Optimization & LTO.
    add_section_title(5, tr("Optimization & LTO"), QStringLiteral("kiTitle3"));
    add_row(6, 0, tr("LTO"), QStringLiteral("kiOlLto"), info.lto_status);
    add_row(6, 1, tr("Optimization"), QStringLiteral("kiOlOptimization"), info.optimization_flag);

    // 4 — Scheduling & Latency.
    add_section_title(7, tr("Scheduling & Latency"), QStringLiteral("kiTitle4"));
    add_row(8, 0, tr("Tick rate"), QStringLiteral("kiSlTickRate"), info.tick_rate);
    add_row(8, 1, tr("sched_ext"), QStringLiteral("kiSlSchedExt"), info.sched_ext);
    add_row(8, 2, tr("Preemption"), QStringLiteral("kiSlPreemption"), info.preemption_model);

    // 5 — Runtime Subsystems.
    add_section_title(9, tr("Runtime Subsystems"), QStringLiteral("kiTitle5"));
    add_row(10, 0, tr("MGLRU"), QStringLiteral("kiOsMglru"), info.mglru);
    add_row(10, 1, tr("THP"), QStringLiteral("kiOsThp"), info.thp);
    add_row(10, 2, tr("TCP congestion"), QStringLiteral("kiOsTcpCongestion"), info.tcp_congestion);
    add_row(11, 0, tr("Clocksource"), QStringLiteral("kiOsClocksource"), info.clocksource);

    // The frame's 150 px-tall slack (the .ui maximumSize) is absorbed by an
    // invisible stretch row below the last value row, so the grid rows stay
    // compact and top-anchored instead of stretching unevenly.
    grid->setRowStretch(12, 1);

    // The natural-height minimum (plan v1.30.0 D1 — the load-bearing line):
    // the scroll area is widgetResizable, so it resizes the frame to the
    // viewport — with the frame's minimum height at 0 a 13-row card is
    // squeezed to the 150 px band and clipped with no scrollbar (the
    // resize rule is qMax(viewport, minimumSize), not the sizeHint).
    // Pinning the minimum to the grid's sizeHint keeps the card at its
    // natural height, makes the band scroll vertically as needed, and
    // re-computes on every idempotent rebuild (the new grid's sizeHint is
    // the source of truth).
    frame->setMinimumHeight(grid->sizeHint().height());
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
    // The install folder is the target's local-dir fact: the gate's
    // headers check also sees a headers package shipped alongside the
    // kernel in that folder (the dir-install pairing — no repo needed).
    target.dir = dir.toStdString();
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
        QMessageBox::information(this, tr("Kernel Manager"), QCoreApplication::translate("MainWindow", qPrintable(QString::fromStdString(v.message))));
        return true;

    case driver_gate::GateAction::WARN_ONLY: {
        bring_window_forward(this);
        QMessageBox box(this);
        box.setIcon(QMessageBox::Warning);
        box.setWindowTitle(tr("Kernel Manager"));
        box.setText(QCoreApplication::translate("MainWindow", qPrintable(QString::fromStdString(v.message))));
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
        box.setText(QCoreApplication::translate("MainWindow", qPrintable(QString::fromStdString(v.message))));
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
    m_driver_banner->setText(QCoreApplication::translate("MainWindow", qPrintable(text)));
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
    // D5 (plan v1.30.0): the guaranteed-full contract — the label's text
    // may now be elided at any width (the shared ElideFilter), so the
    // dynamic property carries the full path; the tooltip above already
    // does (the D6 v1.24.0 contract, preserved + extended).
    m_ui->buildDirLabel->setProperty("km_full_text", QString::fromStdString(path));
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

// Refresh (plan v1.29.0 D2): the manual re-scan — the bottom-row button
// between Configure and Close. Fixed step order:
//   1. Guards (silent no-op): a transaction in flight (m_running — the
//      worker's post-transaction auto-refresh is authoritative) or the
//      Configure clone flow owning the shared progress dialog
//      (m_future_watcher) → return; both mirror on_execute's m_running
//      early-return.
//   2. init_kernels() — the existing full refresh, reused verbatim (the
//      auto-refresh and the manual refresh share one code path, so they
//      are idempotent together): the shared progress dialog with its own
//      label, the alpm re-parse under m_mutex, the Kernel::get_kernels
//      re-fetch, the tree clear + rebuild. If the re-parse fails, its
//      existing path (critical box + hide + early return) leaves the
//      previous tree intact; the purge + header steps below still run
//      (both are data-driven over m_kernels and idempotent — harmless).
//   3. purge_stale_rows() — the D4 stale-row removal over the rebuilt
//      tree.
//   4. build_kernel_info_header() — the idempotent re-extraction (the D3
//      teardown preamble keeps a repeat call at 36 → 36 labels).
// The progress dialog is the shared m_conf_progress_dialog; step 2 hides
// it on every path, so nothing is left open here. Threading: reachable
// only on the main thread (the worker's auto-refresh is a queued
// invokeMethod, the manual click is the event loop) — no new lock
// interaction beyond init_kernels' existing m_mutex use. A rapid
// double-click serializes in the event loop and is a clean no-op
// re-fetch (steps 3–4 are idempotent by construction).
void MainWindow::on_refresh() noexcept {
    if (m_running.load(std::memory_order_relaxed)) {
        return;  // a transaction is in flight — its auto-refresh is authoritative
    }
    if (m_future_watcher.isRunning()) {
        return;  // the Configure clone flow owns the shared progress dialog
    }
    init_kernels();
    purge_stale_rows();
    build_kernel_info_header();
}

// The Refresh flow's stale-row purge (plan v1.29.0 D4): remove the
// rebuilt tree rows whose kernel is neither a real repo/AUR package
// (no has_pkg() — the non-selectable info-rows with the lock glyph) nor
// present in the local DB (no is_installed() — built/folder kernels that
// are no longer installed). This is exactly what lingers after a
// built/folder kernel is uninstalled: the ghost row Refresh clears.
// Data-driven: each row maps to its Kernel in m_kernels by get_raw()
// (raw names are unique per row — the k12 no-duplicate assertion); a row
// with no mapped kernel is never purged (a mismatch is no evidence), and
// cell text is never parsed. The "Install from directory…" pseudo-row
// (D1 v1.24.0) is not a kernel — it has no m_kernels entry and always
// survives every refresh (its comment contract). Never touched: any
// has_pkg() row (a live repo/AUR package) and any is_installed() row
// (an installed kernel, in any class). View-level only: m_kernels, the
// curated list, /etc/pacman.conf, and the local DB are untouched — a
// refresh remains a read-only alpm operation (the k13/k14 system-state
// gates apply). Top-level items are walked bottom-up (a removal shifts
// the indices below it).
//
// Guardrail outcome (D4 implementer-inspection duty) — observed on the
// live machine behind the k12 harness (pre-refresh dump: 18 live +
// 7 info disabled-repo + 0 info enabled-repo + 1 directory): the
// predicate catches exactly the 7 curated info-rows whose repos are
// not enabled in /etc/pacman.conf and which are not installed
// (chaotic-aur/linux-mainline, -xanmod, -xanmod-edge, -xanmod-lts,
// -xanmod-rt, liquorix/linux-lqx, chaotic-aur/linux-clear — all
// Install "—") — outcome (a): the stale residue is exactly the user's
// complaint (uninstalled built/folder ghosts — already dropped by the
// fresh re-fetch's pass 4 — plus disabled-repo info-rows; the purge is
// the backstop for both classes). No installed info-row exists on this
// machine, so no row the user still wants is at risk. A merely-disabled
// repo's row is transient by design (it reappears on the next refresh
// while the state persists — the row's right-click "Add repo" or
// `pacman -Sy` is the permanent resolution, design doc Edge Cases).
void MainWindow::purge_stale_rows() noexcept {
    auto* tree_kernels = m_ui->treeKernels;
    for (int r = tree_kernels->topLevelItemCount() - 1; r >= 0; --r) {
        auto* item            = tree_kernels->topLevelItem(r);
        const QString pkg_raw = item->text(static_cast<int>(TreeCol::PkgName));
        // The directory pseudo-row (D1 v1.24.0) is not a kernel — it
        // must survive every refresh (its comment contract).
        if (pkg_raw == tr(kDirectoryRowRaw)) {
            continue;
        }
        const auto it = std::ranges::find_if(m_kernels, [&pkg_raw](const Kernel& k) { return k.get_raw() == pkg_raw.toStdString(); });
        if (it == m_kernels.end()) {
            continue;  // no mapped kernel — never purge on a mismatch
        }
        if (!it->has_pkg() && !it->is_installed()) {
            // Detach + delete the row's item widget (the info-row lock
            // QLabel) before the row goes: deleting the item alone
            // orphans the widget on the tree (Qt never deletes an item
            // widget — probe P2a), and removeItemWidget cleans the
            // tree's item-widget hash entry first (probe P2b), so no
            // later clear() dereferences a dangling widget.
            for (int c = 0; c < item->columnCount(); ++c) {
                if (auto* w = tree_kernels->itemWidget(item, c)) {
                    tree_kernels->removeItemWidget(item, c);
                    delete w;
                }
            }
            tree_kernels->takeTopLevelItem(r);
            delete item;
        }
    }
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
