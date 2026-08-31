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

// Offscreen UI driver for the chunk-2 source dropdown (conf-window.cpp is
// included so the window can be driven directly; no CTest infra in this
// project — run via tests/run_chunk2_ui.sh). Verifies:
//   - the combo holds the known sources + the trailing "Custom URL…" entry
//   - applying "core/linux" auto-populates the "linux" source
//     (identity mapping) and keeps the custom edit hidden
//   - re-applying the same kernel is a no-op (manual choice preserved)
//   - switching to "aur/linux-zen" maps to "linux-zen"
//   - an unknown kernel falls back to "Custom URL…" prefilled with its name
//   - a typed custom URL round-trips through sync_build_source() into the
//     single utils accessor
//   - the 17 option rows follow the per-source option set resolved from
//     the selected build source (per-source row-visibility matrix)
//   - get_all_set_values() emits the per-source values through the
//     generated map + value translation (xanmod: exactly _localmodcfg=y +
//     _microarchitecture=99; cachyos: _HZ_ticks=300 + _tcp_bbr3=yes)
//
// Test-only access trick (strict pre-include variant): every transitive
// header is pre-included cleanly below, then the hack flips the access
// specifiers of the ConfWindow class BODY only (its includes are all
// no-ops at that point) so the private set_build_source() /
// get_all_set_values() can be driven offscreen. The naive whole-TU form
// of the hack breaks GCC 16 (e.g. <sstream>).

#include <ui_conf-window.h>
#include <alpm_utils.hpp>
#include <string_utils.hpp>
#include <config-options.hpp>
#include <known_kernels.hpp>
#include <utils.hpp>
#include <compile_options.hpp>
#include <fmt/compile.h>
#include <fmt/core.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>

#include <QtCore/QtCore>
#include <QtGui/QtGui>
#include <QtWidgets/QtWidgets>
#include <QtConcurrent/QtConcurrent>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSignalBlocker>
#include <QString>
#include <QStringList>

#define private public
#define protected public
#include "conf-window.hpp"
#undef private
#undef protected

#include "conf-window.cpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) {
        std::printf("PASS: %s\n", what);
    } else {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app{argc, argv};

    ConfWindow conf;
    auto* combo       = conf.findChild<QComboBox*>("build_source_combo");
    auto* custom_edit = conf.findChild<QLineEdit*>("build_source_custom_edit");
    check(combo != nullptr && custom_edit != nullptr, "source combo + custom edit present");
    if (combo == nullptr || custom_edit == nullptr) {
        return 1;
    }

    // The dropdown carries the known sources + the trailing "Custom URL…".
    check(combo->count() == static_cast<int>(km::known_sources().size()) + 1, "combo holds the known sources + Custom URL…");
    check(combo->itemText(combo->count() - 1).toStdString() == "Custom URL…", "trailing entry is Custom URL…");

    // NOTE: the window itself is never shown in this offscreen driver, so
    // chain-dependent QWidget::isVisible() is not used; the edit's own
    // hidden state (WA_WState_Hidden, set by setVisible) is asserted.
    // 1. core/linux -> the "linux" source (identity mapping).
    conf.apply_source_for_kernel("core/linux");
    check(combo->currentText().toStdString() == "linux", "core/linux auto-populates the combo with linux");
    check(custom_edit->isHidden(), "custom edit hidden for a known source");
    conf.sync_build_source();
    check(utils::build_source_repo() == "linux", "sync_build_source persists linux");

    // 2. Re-applying the same kernel must not clobber a manual choice.
    combo->setCurrentIndex(combo->count() - 1);  // Custom URL…
    custom_edit->setText("my-manual-source");
    check(!custom_edit->isHidden(), "selecting Custom URL… reveals the edit");
    conf.apply_source_for_kernel("core/linux");  // same kernel -> no-op
    check(combo->currentText().toStdString() == "Custom URL…" && custom_edit->text().toStdString() == "my-manual-source",
        "repeat apply for the same kernel is a no-op (manual choice kept)");

    // 3. Switching to another known kernel maps it (linux-zen -> linux-zen).
    conf.apply_source_for_kernel("aur/linux-zen");
    check(combo->currentText().toStdString() == "linux-zen", "aur/linux-zen auto-populates the combo with linux-zen");
    check(custom_edit->isHidden(), "custom edit hidden again for a known source");

    // 4. Unknown kernel -> "Custom URL…" prefilled with the kernel name.
    conf.apply_source_for_kernel("linux-rc");
    check(combo->currentText().toStdString() == "Custom URL…", "unknown kernel selects Custom URL…");
    check(!custom_edit->isHidden() && custom_edit->text().toStdString() == "linux-rc", "unknown kernel prefills the edit with its name");

    // 5. Custom URL round-trips through the utils accessor.
    custom_edit->setText("https://example.com/my-kernel.git");
    conf.sync_build_source();
    check(utils::build_source_repo() == "https://example.com/my-kernel.git", "custom URL round-trips through sync_build_source");

    // 6. Per-source 17-row visibility matrix: the option rows
    // (option_row_bindings) follow the per-repo option set resolved from
    // the selected build source; isHidden() is each row's own hidden state
    // (parent-independent, so it is asserted although the window is not shown).
    auto* page_ui = conf.m_ui->conf_options_page_widget->get_ui_obj();
    auto visible_count = [page_ui]() {
        int n{0};
        for (const auto& [option, row] : option_row_bindings) {
            QWidget* w = page_ui->*row;  // the row's own hidden state (parent-independent)
            if (!w->isHidden()) {
                ++n;
            }
        }
        return n;
    };
    auto row_visible = [page_ui](std::string_view option) {
        for (const auto& [key, row] : option_row_bindings) {
            if (key == option) {
                QWidget* w = page_ui->*row;
                return !w->isHidden();
            }
        }
        return false;
    };

    conf.set_build_source("linux-cachyos");
    check(visible_count() == 17, "linux-cachyos: all 17 option rows visible (0 hidden)");

    conf.set_build_source("linux-cachyos-bore");
    check(visible_count() == 17, "linux-cachyos-bore: all 17 option rows visible (0 hidden)");

    conf.set_build_source("linux-xanmod");
    check(visible_count() == 2 && row_visible("cpu_opt") && row_visible("localmodcfg"),
        "linux-xanmod: exactly the cpu_opt + localmodcfg rows visible (15 hidden)");

    conf.set_build_source("linux-lqx");
    check(visible_count() == 1 && row_visible("localmodcfg"),
        "linux-lqx: exactly the localmodcfg row visible (16 hidden)");

    conf.set_build_source("linux");
    check(visible_count() == 0, "linux: no option rows visible (17 hidden)");

    conf.set_build_source("https://example.com/unknown-kernel.git");
    check(visible_count() == 0, "custom/unknown source: no option rows visible (17 hidden)");

    // 7. get_all_set_values() emission: every offered option passes
    // through the per-repo map + generated value translation.
    auto* cpu_combo = page_ui->processor_opt_combo_box;
    auto* hz_combo  = page_ui->hzticks_combo_box;
    auto* lmc_check = page_ui->localmodcfg_check;
    auto* bbr_check = page_ui->tcpbbr_check;

    conf.set_build_source("linux-xanmod");
    conf.sync_build_source();
    cpu_combo->setCurrentIndex(1);  // Native CPU
    lmc_check->setCheckState(Qt::Checked);
    {
        const std::string out = conf.get_all_set_values();
        check(out == "_localmodcfg=y\n_microarchitecture=99\n",
            "xanmod cpu_opt=native + localmodcfg on: exactly _localmodcfg=y + _microarchitecture=99, no other vars");
    }

    conf.set_build_source("linux-cachyos");
    conf.sync_build_source();
    hz_combo->setCurrentIndex(4);  // 300Hz
    bbr_check->setCheckState(Qt::Checked);
    {
        const std::string out = conf.get_all_set_values();
        check(out.find("_HZ_ticks=300\n") != std::string::npos, "cachyos hzticks=300: emits _HZ_ticks=300");
        check(out.find("_tcp_bbr3=yes\n") != std::string::npos, "cachyos bbr3 on: emits _tcp_bbr3=yes");
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
