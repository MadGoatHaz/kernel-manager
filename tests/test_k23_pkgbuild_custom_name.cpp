// Copyright (C) 2022-2026 Vladislav Nepogodin
//
// This file is part of kernel-manager.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// K23: unit tests for utils::apply_pkgbuild_custom_name — the generic
// PKGBUILD custom-name rewrite (T-044). This is the function that lets a
// user build a custom-named kernel package: it rewrites the first
// top-level pkgbase= line in a PKGBUILD with a user-supplied name,
// expanding a "$pkgbase" / "${pkgbase}" placeholder against the file's
// original pkgbase value. The rewrite must be idempotent (repeat runs
// never accumulate suffixes), must reject shell metacharacters in the
// custom name (injection guard), and must fail cleanly on a PKGBUILD
// lacking the expected structure (never corrupt a foreign file).
//
// Coverage:
//   A. basic bare rename: pkgbase="linux-zen" + "my-kernel" →
//      pkgbase="my-kernel"
//   B. $pkgbase placeholder: pkgbase="linux" + "$pkgbase-custom" →
//      pkgbase="linux-custom"
//   C. ${pkgbase} braced placeholder: pkgbase="linux" +
//      "${pkgbase}-custom" → pkgbase="linux-custom"
//   D. mixed placeholder: pkgbase="linux" + "custom-$pkgbase-more" →
//      pkgbase="custom-linux-more"
//   E. idempotency: applying "${pkgbase}-custom" to a file whose pkgbase
//      is already "linux-custom" yields "linux-custom" again (no
//      -custom-custom growth)
//   F. idempotency (double-stacked): a file whose pkgbase is
//      "linux-custom-custom" (two previous failed-checkout rewrites)
//      collapses back to "linux-custom" on a third application
//   G. empty custom name: no-op, content unchanged, ok=true
//   H. identity name "$pkgbase": no-op, content unchanged, ok=true
//   I. identity name "${pkgbase}": no-op, content unchanged, ok=true
//   J. invalid character (semicolon): ok=false, content untouched
//   K. invalid character (backtick): ok=false, content untouched
//   L. invalid character (dollar-quote): ok=false, content untouched
//   M. missing pkgver= / pkgver(): ok=false, content untouched
//   N. missing pkgbase=: ok=false, content untouched
//   O. pkgver() function form recognized as valid structure
//   P. multi-line PKGBUILD: only the first top-level pkgbase= is
//      rewritten, subsequent assignments untouched
//
// Recipe: standalone g++ against the real utils.cpp (run_k7.sh
// precedent), linking fmt + glib + Qt6Core + libalpm. The function is
// pure (takes content, returns a result struct), so no filesystem,
// QSettings, or alpm state is involved — the test is entirely in-memory.

#include <string>

#include <fmt/core.h>

#include "utils.hpp"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (cond) {                                                          \
            g_pass++;                                                        \
        } else {                                                             \
            g_fail++;                                                        \
            fmt::print(stderr, "K23 FAIL [{}:{}] {}\n", __FILE__, __LINE__,  \
                       msg);                                                \
        }                                                                    \
    } while (0)

// A minimal, well-formed PKGBUILD skeleton for the positive tests.
static const std::string PKGBUILD_BASE =
    "# Maintainer: Test <test@example.com>\n"
    "pkgbase=linux-zen\n"
    "pkgname=($pkgbase $pkgbase-headers)\n"
    "pkgver=6.14.7\n"
    "pkgrel=1\n"
    "pkgdesc='The Linux kernel and modules'\n"
    "arch=(x86_64)\n"
    "url='https://www.kernel.org/'\n"
    "license=(GPL-2.0)\n"
    "source=($url/linux-$pkgver.tar.xz::linux)\n"
    "md5sums=('SKIP')\n"
    "prepare() {\n"
    "    cd $srcdir/linux\n"
    "}\n"
    "build() {\n"
    "    cd $srcdir/linux\n"
    "}\n";

int main() {
    // -----------------------------------------------------------------
    // A. Basic bare rename
    // -----------------------------------------------------------------
    {
        auto result = utils::apply_pkgbuild_custom_name(PKGBUILD_BASE, "my-kernel");
        CHECK(result.ok, "A: bare rename must succeed");
        CHECK(result.new_content.find("pkgbase=\"my-kernel\"") != std::string::npos,
              "A: pkgbase rewritten to my-kernel");
        CHECK(result.new_content.find("pkgbase=linux-zen") == std::string::npos,
              "A: original pkgbase gone");
    }

    // -----------------------------------------------------------------
    // B. $pkgbase placeholder
    // -----------------------------------------------------------------
    {
        auto result = utils::apply_pkgbuild_custom_name(PKGBUILD_BASE, "$pkgbase-custom");
        CHECK(result.ok, "B: $pkgbase placeholder must succeed");
        CHECK(result.new_content.find("pkgbase=\"linux-zen-custom\"") != std::string::npos,
              "B: pkgbase rewritten to linux-zen-custom");
    }

    // -----------------------------------------------------------------
    // C. ${pkgbase} braced placeholder
    // -----------------------------------------------------------------
    {
        auto result = utils::apply_pkgbuild_custom_name(PKGBUILD_BASE, "${pkgbase}-custom");
        CHECK(result.ok, "C: ${pkgbase} placeholder must succeed");
        CHECK(result.new_content.find("pkgbase=\"linux-zen-custom\"") != std::string::npos,
              "C: pkgbase rewritten to linux-zen-custom");
    }

    // -----------------------------------------------------------------
    // D. Mixed placeholder
    // -----------------------------------------------------------------
    {
        auto result = utils::apply_pkgbuild_custom_name(PKGBUILD_BASE, "custom-$pkgbase-more");
        CHECK(result.ok, "D: mixed placeholder must succeed");
        CHECK(result.new_content.find("pkgbase=\"custom-linux-zen-more\"") != std::string::npos,
              "D: pkgbase rewritten to custom-linux-zen-more");
    }

    // -----------------------------------------------------------------
    // E. Idempotency: applying to an already-renamed file
    // -----------------------------------------------------------------
    {
        // First application: linux-zen → linux-zen-custom
        auto first = utils::apply_pkgbuild_custom_name(PKGBUILD_BASE, "${pkgbase}-custom");
        CHECK(first.ok, "E.1: first application must succeed");
        CHECK(first.new_content.find("pkgbase=\"linux-zen-custom\"") != std::string::npos,
              "E.1: first application result");

        // Second application on the result: linux-zen-custom → linux-zen-custom
        // (the peel recovers linux-zen, then re-applies -custom)
        auto second = utils::apply_pkgbuild_custom_name(first.new_content, "${pkgbase}-custom");
        CHECK(second.ok, "E.2: second application must succeed");
        CHECK(second.new_content == first.new_content,
              "E.2: idempotent — second application yields identical content");
    }

    // -----------------------------------------------------------------
    // F. Idempotency: double-stacked suffix collapses
    // -----------------------------------------------------------------
    {
        // Simulate a file that was renamed twice (e.g., two failed
        // checkouts): pkgbase=linux-zen-custom-custom
        std::string double_stacked = PKGBUILD_BASE;
        double_stacked.replace(0, double_stacked.find('\n'), "pkgbase=linux-zen-custom-custom\n");

        auto result = utils::apply_pkgbuild_custom_name(double_stacked, "${pkgbase}-custom");
        CHECK(result.ok, "F: double-stacked peel must succeed");
        CHECK(result.new_content.find("pkgbase=\"linux-zen-custom\"") != std::string::npos,
              "F: double-stacked collapses to single -custom");
    }

    // -----------------------------------------------------------------
    // G. Empty custom name: no-op
    // -----------------------------------------------------------------
    {
        auto result = utils::apply_pkgbuild_custom_name(PKGBUILD_BASE, "");
        CHECK(result.ok, "G: empty name must succeed (no-op)");
        CHECK(result.new_content == PKGBUILD_BASE,
              "G: empty name leaves content unchanged");
    }

    // -----------------------------------------------------------------
    // H. Identity name "$pkgbase": no-op
    // -----------------------------------------------------------------
    {
        auto result = utils::apply_pkgbuild_custom_name(PKGBUILD_BASE, "$pkgbase");
        CHECK(result.ok, "H: $pkgbase identity must succeed (no-op)");
        CHECK(result.new_content == PKGBUILD_BASE,
              "H: $pkgbase identity leaves content unchanged");
    }

    // -----------------------------------------------------------------
    // I. Identity name "${pkgbase}": no-op
    // -----------------------------------------------------------------
    {
        auto result = utils::apply_pkgbuild_custom_name(PKGBUILD_BASE, "${pkgbase}");
        CHECK(result.ok, "I: ${pkgbase} identity must succeed (no-op)");
        CHECK(result.new_content == PKGBUILD_BASE,
              "I: ${pkgbase} identity leaves content unchanged");
    }

    // -----------------------------------------------------------------
    // J. Invalid character (semicolon)
    // -----------------------------------------------------------------
    {
        auto result = utils::apply_pkgbuild_custom_name(PKGBUILD_BASE, "bad;name");
        CHECK(!result.ok, "J: semicolon must be rejected");
        CHECK(result.new_content.empty(), "J: content untouched on rejection");
    }

    // -----------------------------------------------------------------
    // K. Invalid character (backtick)
    // -----------------------------------------------------------------
    {
        auto result = utils::apply_pkgbuild_custom_name(PKGBUILD_BASE, "bad`name");
        CHECK(!result.ok, "K: backtick must be rejected");
        CHECK(result.new_content.empty(), "K: content untouched on rejection");
    }

    // -----------------------------------------------------------------
    // L. Invalid character (double quote)
    // -----------------------------------------------------------------
    {
        auto result = utils::apply_pkgbuild_custom_name(PKGBUILD_BASE, "bad\"name");
        CHECK(!result.ok, "L: double-quote must be rejected");
        CHECK(result.new_content.empty(), "L: content untouched on rejection");
    }

    // -----------------------------------------------------------------
    // M. Missing pkgver= / pkgver()
    // -----------------------------------------------------------------
    {
        std::string no_pkgver =
            "pkgbase=linux-zen\n"
            "pkgrel=1\n"
            "pkgdesc='The Linux kernel and modules'\n"
            "prepare() {\n"
            "}\n";
        auto result = utils::apply_pkgbuild_custom_name(no_pkgver, "my-kernel");
        CHECK(!result.ok, "M: missing pkgver must be rejected");
        CHECK(result.new_content.empty(), "M: content untouched on rejection");
    }

    // -----------------------------------------------------------------
    // N. Missing pkgbase=
    // -----------------------------------------------------------------
    {
        std::string no_pkgbase =
            "pkgname=linux-zen\n"
            "pkgver=6.14.7\n"
            "pkgrel=1\n"
            "pkgdesc='The Linux kernel and modules'\n"
            "prepare() {\n"
            "}\n";
        auto result = utils::apply_pkgbuild_custom_name(no_pkgbase, "my-kernel");
        CHECK(!result.ok, "N: missing pkgbase must be rejected");
        CHECK(result.new_content.empty(), "N: content untouched on rejection");
    }

    // -----------------------------------------------------------------
    // O. pkgver() function form recognized as valid structure
    // -----------------------------------------------------------------
    {
        std::string pkgver_func =
            "pkgbase=linux-zen\n"
            "pkgname=($pkgbase)\n"
            "pkgver() {\n"
            "    cd $srcdir/linux\n"
            "    make -s kernelrelease\n"
            "}\n"
            "pkgrel=1\n"
            "prepare() {\n"
            "}\n";
        auto result = utils::apply_pkgbuild_custom_name(pkgver_func, "my-kernel");
        CHECK(result.ok, "O: pkgver() function form must be accepted");
        CHECK(result.new_content.find("pkgbase=\"my-kernel\"") != std::string::npos,
              "O: pkgbase rewritten");
    }

    // -----------------------------------------------------------------
    // P. Multi-line PKGBUILD: only the first top-level pkgbase= rewritten
    // -----------------------------------------------------------------
    {
        std::string multi_pkgbase =
            "# A comment\n"
            "pkgbase=linux-zen\n"
            "pkgver=6.14.7\n"
            "pkgrel=1\n"
            "prepare() {\n"
            "    # nested pkgbase inside a function must NOT be touched\n"
            "    pkgbase=should-not-change\n"
            "}\n";
        auto result = utils::apply_pkgbuild_custom_name(multi_pkgbase, "my-kernel");
        CHECK(result.ok, "P: multi-line PKGBUILD must succeed");
        CHECK(result.new_content.find("pkgbase=\"my-kernel\"") != std::string::npos,
              "P: first top-level pkgbase rewritten");
        CHECK(result.new_content.find("pkgbase=should-not-change") != std::string::npos,
              "P: nested pkgbase untouched");
        CHECK(result.new_content.find("pkgbase=linux-zen") == std::string::npos,
              "P: original pkgbase gone");
    }

    fmt::print("K23: {} passed, {} failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
