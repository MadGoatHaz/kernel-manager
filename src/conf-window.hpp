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

#ifndef CONFWINDOW_HPP_
#define CONFWINDOW_HPP_

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
#pragma GCC diagnostic ignored "-Wsuggest-final-methods"
#pragma GCC diagnostic ignored "-Wsuggest-attribute=pure"
#endif

#include <ui_conf-window.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QMainWindow>
#include <QProcess>
#include <QTimer>

// A kernel build flavor discovered in the kernel source repo:
// key = flavor name relative to the repo's family prefix, path = the
// flavor's ABSOLUTE on-disk directory (repo root / subdirectory; for the
// single-package layout — a PKGBUILD at the repo root — the repo root
// itself).
struct KernelFlavor {
    std::string key;
    std::string path;
};

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

class ConfWindow final : public QMainWindow {
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(ConfWindow)
 public:
    explicit ConfWindow(QWidget* parent = nullptr);
    ~ConfWindow() = default;

    void reset_patches_data_tab() noexcept;
    void refresh_flavors() noexcept;

    // Apply the build source selected in the UI (AUR package name or git
    // URL) to the single utils accessor used by prepare_build_environment().
    void sync_build_source() noexcept;

    // Auto-populate the build source dropdown from a kernel selected in the
    // main window tree (raw "repo/name" PkgName): the repo prefix is
    // stripped and the known-kernels table maps the name to its default
    // source (e.g. "core/linux" -> "linux-cachyos"); a source that is not a
    // known one falls back to the "Custom URL…" entry prefilled. Repeated
    // calls for the same kernel are a no-op, so a manual source choice is
    // preserved while the user stays on that kernel.
    void apply_source_for_kernel(std::string_view kernel_name) noexcept;

    protected:
    void closeEvent(QCloseEvent* event) override;

    private:
    void on_cancel() noexcept;
    void on_execute() noexcept;
    void on_save() noexcept;
    void on_load() noexcept;
    void finished_proc(int exit_code, QProcess::ExitStatus exit_status) noexcept;
    // Build completion (cycle-7 C3/D3): the success path shared by the
    // finished_proc marker hit and the bounded done-status poll; the poll
    // timer's timeout slot.
    void handle_build_done() noexcept;
    void on_done_status_tick() noexcept;

    auto kernel_path_for_index(std::int32_t index) const noexcept -> std::string;
    void update_option_set() noexcept;

    // Source dropdown plumbing: show/hide the custom URL edit per selection
    // and keep the option rows in sync; set the effective source from a
    // string (known item or custom entry); read the effective source the
    // user currently has selected.
    void update_build_source_ui() noexcept;
    void set_build_source(const std::string& source) noexcept;
    auto effective_build_source() const noexcept -> std::string;

    bool m_running{};
    QProcess m_cmd{};
    std::string m_build_conf_path{};
    std::vector<std::string> m_previously_set_options{};
    std::vector<KernelFlavor> m_flavors{};

    // Last kernel name (repo prefix stripped) auto-populated via
    // apply_source_for_kernel; a repeat for the same kernel is a no-op.
    std::string m_last_applied_kernel{};

    // Build completion tracking (cycle-7 C3/D3): the terminal-helper's
    // lifetime equals the launched command's lifetime (the D2 contract), so
    // m_cmd's "finished" event fires when the command ends; the .done-status
    // marker (touched by the build command) is the completion signal for
    // expect_done launches, marker-less launches (the post-build pacman -U,
    // escalated ops) end normally without one, and the timer is the bounded
    // belt-and-braces poll if the marker lands slightly after the helper
    // exits.
    QTimer m_done_status_timer{this};
    bool m_expect_done{true};
    int m_last_helper_rc{0};
    std::size_t m_done_polls_left{0};

    std::unique_ptr<Ui::ConfWindow> m_ui = std::make_unique<Ui::ConfWindow>();

    void run_cmd_async(std::string cmd, const std::string& working_path, bool escalate = false, bool expect_done = true) noexcept;
    auto get_all_set_values() const noexcept -> std::string;
    void clear_patches_data_tab() noexcept;
    void connect_all_checkboxes() noexcept;
};

#endif  // CONFWINDOW_HPP_
