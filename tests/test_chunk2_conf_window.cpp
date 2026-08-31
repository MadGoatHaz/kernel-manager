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

#include "conf-window.cpp"

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>

#include <cstdio>   // for printf
#include <cstdlib>  // for exit

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

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
