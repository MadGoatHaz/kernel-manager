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

// k15: build-dir persistence harness (D4+D5) — the test half of chunk C8.
// Standalone g++ recipe (tests/run_k7.sh precedent) against the real
// src/utils.cpp + src/aur_kernel.cpp, with the project's GCC warning set;
// links fmt + glib + Qt6Core + libalpm. See tests/run_k15.sh for the
// runner and its gates.
//
// D4 harness seam: main() pins XDG_CONFIG_HOME to a sandbox via setenv
// BEFORE any utils:: call / QSettings construction, so the settings file
// (ArchLinux/KernelManager.*, the extension resolved via
// QSettings::fileName() — never hardcoded, C5 AHEAD note) can never reach
// the user's real config. When the runner exports a sandbox it is reused
// (the runner's trap owns its cleanup); otherwise a private sandbox is
// created here and removed on exit, so the binary is safe standalone too.
//
// Parts (in execution order):
//   (A) default — fresh sandbox, no file yet: build_repo_path() == the
//       expanded <home>/.cache/kernel-manager/pkgbuilds (home computed
//       from g_get_home_dir — no hardcoded home), build_app_path() ==
//       <home>/.cache/kernel-manager, detail::aur_pkgbuilds_path() ==
//       <default>/aur_pkgbuilds (and +"/linux-zen" for a named package,
//       the C6 seam)
//   (E) stability — still no file: the default is byte-identical to the
//       pre-C5 literal `~/.cache/kernel-manager/pkgbuilds` expanded
//       (zero-behavior-change gate for existing users); placed with A
//       because a once-set value can only be replaced, never cleared
//       (the no-op semantics), so both need the pristine state
//   (B) set → read — set_build_dir("/tmp/kmtest") reflects immediately
//       (no cache, D4): build_repo_path()/build_app_path()/
//       aur_pkgbuilds_path() + the file now exists under the sandbox and
//       contains buildDir=/tmp/kmtest (read back via
//       utils::read_whole_file — the real persistence artifact, not just
//       the QSettings API)
//   (C) persistence — an independent QSettings("ArchLinux",
//       "KernelManager") instance re-reads the file and gets
//       buildDir == /tmp/kmtest (the cross-process contract); the
//       accessor still agrees
//   (D) `~` expansion + no-ops — "~/km-builddir" is stored EXPANDED (a
//       stored ~ would be ambiguous across users); "" and "   \t " are
//       no-ops (previous value kept, the set_build_source_repo pattern);
//       "relative/dir" is stored as-is (the picker always returns
//       absolute; the accessor is a dumb persist)
#include "aur_kernel.hpp"  // for detail::aur_pkgbuilds_path
#include "utils.hpp"       // for build_repo_path, build_app_path, set_build_dir, fix_path, read_whole_file

#include <cstdio>      // for printf, fprintf
#include <cstdlib>     // for setenv, getenv
#include <filesystem>  // for path, exists, temp_directory_path, create_directories, remove_all
#include <string>      // for string
#include <unistd.h>    // for getpid

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wold-style-cast"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wnull-dereference"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wsuggest-attribute=pure"
#endif

#include <QSettings>  // for QSettings
#include <glib.h>     // for g_get_home_dir

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fs = std::filesystem;

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

void check_path(const fs::path& actual, const fs::path& expected, const char* what) {
    if (actual == expected) {
        std::printf("PASS: %s\n", what);
    } else {
        ++g_failures;
        std::printf("FAIL: %s (actual=%s expected=%s)\n", what, actual.c_str(), expected.c_str());
    }
}

// The settings file this harness talks to: explicit org/app (D4) resolved
// against the pinned XDG_CONFIG_HOME sandbox. The filename (and its
// extension) comes from Qt itself — KernelManager.conf on this Qt build —
// and is never hardcoded.
std::string settings_file() {
    return QSettings{"ArchLinux", "KernelManager"}.fileName().toStdString();
}

}  // namespace

int main() {
    // D4 harness seam: pin XDG_CONFIG_HOME before any utils:: call.
    // Reuse the runner's sandbox when exported (the runner's trap owns
    // its cleanup); otherwise create a private one and remove it on exit.
    const char* inherited = std::getenv("XDG_CONFIG_HOME");
    std::string sandbox{};
    bool own_sandbox = false;
    if (inherited != nullptr && inherited[0] != '\0') {
        sandbox = inherited;
    } else {
        sandbox = (fs::temp_directory_path() / ("km15-sandbox-" + std::to_string(static_cast<long>(::getpid())))).string();
        std::error_code ec{};
        fs::create_directories(sandbox, ec);
        if (ec) {
            std::fprintf(stderr, "k15: cannot create sandbox %s: %s\n", sandbox.c_str(), ec.message().c_str());
            return 1;
        }
        own_sandbox = true;
    }
    if (::setenv("XDG_CONFIG_HOME", sandbox.c_str(), 1) != 0) {  // POSIX setenv (global, like utils.cpp's ::setenv)
        std::fprintf(stderr, "k15: setenv XDG_CONFIG_HOME=%s failed\n", sandbox.c_str());
        return 1;
    }
    std::printf("INFO: XDG_CONFIG_HOME sandbox = %s\n", sandbox.c_str());

    const std::string home = g_get_home_dir() != nullptr ? g_get_home_dir() : "";
    check(!home.empty(), "A: g_get_home_dir() reports a home directory");
    if (home.empty()) {
        return g_failures;
    }

    // The file must resolve inside the sandbox: XDG isolation holds.
    const std::string ini = settings_file();
    check(ini.rfind(sandbox, 0) == 0, "A: QSettings file resolves inside the XDG_CONFIG_HOME sandbox");
    check(fs::path{ini}.parent_path() == fs::path{sandbox} / "ArchLinux", "A: ... under <sandbox>/ArchLinux/ (Qt-resolved name, extension not hardcoded)");

    // ------------------------------------------------------------------
    // (A) default — fresh sandbox, no file yet.
    // ------------------------------------------------------------------
    check(!fs::exists(ini), "A: no settings file in the fresh sandbox");
    const fs::path def_repo = fs::path{home} / ".cache" / "kernel-manager" / "pkgbuilds";
    const fs::path def_app  = fs::path{home} / ".cache" / "kernel-manager";
    check_path(utils::build_repo_path(), def_repo, "A: default build_repo_path() == <home>/.cache/kernel-manager/pkgbuilds");
    check_path(utils::build_app_path(), def_app, "A: default build_app_path() == <home>/.cache/kernel-manager");
    check_path(detail::aur_pkgbuilds_path(), def_repo / "aur_pkgbuilds", "A: default aur_pkgbuilds_path() == <default>/aur_pkgbuilds");
    check_path(detail::aur_pkgbuilds_path("linux-zen"), def_repo / "aur_pkgbuilds" / "linux-zen", "A: aur_pkgbuilds_path(\"linux-zen\") == <default>/aur_pkgbuilds/linux-zen");

    // ------------------------------------------------------------------
    // (E) stability — still no file: byte-identical to the pre-C5 literal
    // expanded (zero-behavior-change gate for existing users).
    // ------------------------------------------------------------------
    const fs::path legacy = utils::fix_path(std::string{"~/.cache/kernel-manager/pkgbuilds"});
    check_path(utils::build_repo_path(), legacy, "E: no-ini default == fix_path(\"~/.cache/kernel-manager/pkgbuilds\") (the pre-C5 literal)");
    check_path(legacy, def_repo, "E: the pre-C5 literal expands to <home>/.cache/kernel-manager/pkgbuilds");

    // ------------------------------------------------------------------
    // (B) set → read — immediate reflection, no cache.
    // ------------------------------------------------------------------
    utils::set_build_dir("/tmp/kmtest");
    check_path(utils::build_repo_path(), fs::path{"/tmp/kmtest"}, "B: set_build_dir(\"/tmp/kmtest\") → build_repo_path() == /tmp/kmtest (immediate, no cache)");
    check_path(utils::build_app_path(), fs::path{"/tmp"}, "B: → build_app_path() == /tmp");
    check_path(detail::aur_pkgbuilds_path(), fs::path{"/tmp/kmtest/aur_pkgbuilds"}, "B: → aur_pkgbuilds_path() == /tmp/kmtest/aur_pkgbuilds");
    check_path(detail::aur_pkgbuilds_path("linux-zen"), fs::path{"/tmp/kmtest/aur_pkgbuilds/linux-zen"}, "B: → aur_pkgbuilds_path(\"linux-zen\") == /tmp/kmtest/aur_pkgbuilds/linux-zen");
    const std::string ini_b = settings_file();
    check(ini_b == ini, "B: settings file location stable across calls");
    check(fs::exists(ini_b), "B: settings file created under the sandbox after set_build_dir");
    const std::string content_b = utils::read_whole_file(ini_b);
    check(content_b.find("buildDir=/tmp/kmtest") != std::string::npos, "B: file content contains buildDir=/tmp/kmtest");

    // ------------------------------------------------------------------
    // (C) persistence — an independent QSettings instance re-reads the
    // file (the cross-process contract); the accessor still agrees.
    // ------------------------------------------------------------------
    {
        QSettings other{"ArchLinux", "KernelManager"};
        check(other.value("buildDir").toString().toStdString() == "/tmp/kmtest", "C: second QSettings instance reads buildDir == /tmp/kmtest");
    }
    check_path(utils::build_repo_path(), fs::path{"/tmp/kmtest"}, "C: accessor still agrees after the independent read");

    // ------------------------------------------------------------------
    // (D) `~` expansion + no-ops + relative stored as-is.
    // ------------------------------------------------------------------
    utils::set_build_dir("~/km-builddir");
    check_path(utils::build_repo_path(), fs::path{home} / "km-builddir", "D: set \"~/km-builddir\" → build_repo_path() == <home>/km-builddir (expanded)");
    {
        QSettings other{"ArchLinux", "KernelManager"};
        check(other.value("buildDir").toString().toStdString() == home + "/km-builddir", "D: the file stores the EXPANDED path (no stored ~)");
    }
    const std::string content_d = utils::read_whole_file(settings_file());
    check(content_d.find("buildDir=" + home + "/km-builddir") != std::string::npos, "D: file content carries buildDir=<home>/km-builddir");

    utils::set_build_dir("");
    check_path(utils::build_repo_path(), fs::path{home} / "km-builddir", "D: set \"\" is a no-op (previous value kept)");
    utils::set_build_dir("   \t ");
    check_path(utils::build_repo_path(), fs::path{home} / "km-builddir", "D: set \"   \\t \" is a no-op (previous value kept)");

    utils::set_build_dir("relative/dir");
    {
        QSettings other{"ArchLinux", "KernelManager"};
        check(other.value("buildDir").toString().toStdString() == "relative/dir", "D: a relative path is stored as-is (the picker returns absolute; the accessor is a dumb persist)");
    }
    check_path(utils::build_repo_path(), fs::path{"relative/dir"}, "D: build_repo_path() returns the stored relative path unmodified");

    // ------------------------------------------------------------------
    const int rc = g_failures;
    std::printf(rc == 0 ? "K15: all checks passed (0 failures)\n" : "K15: %d FAILURES\n", rc);
    if (own_sandbox) {
        std::error_code ec{};
        fs::remove_all(sandbox, ec);
    }
    return rc;
}
