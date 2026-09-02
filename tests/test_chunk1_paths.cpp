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

// Standalone unit test for the chunk-1 path logic (no CTest infra in this
// project; run via tests/run_chunk1.sh, which compiles the REAL sources —
// conf-window.cpp is included here so its file-local path helpers are
// visible, utils.cpp / config-options.cpp are linked as separate TUs).
//
// Covers the custom-build runtime crash:
//   - KernelFlavor::path is absolute (subdir flavors -> repo_root/dir,
//     single-PKGBUILD fallback -> repo root itself)
//   - prepare_git_repo restores the caller's CWD on every exit path
//     (success, clone failure, and re-prepare of an existing repo)
//   - insert_new_source_array_into_pkgbuild does not write on a missing
//     or unreadable PKGBUILD
//   - the PKGBUILD rewrites are idempotent: repeat runs of
//     insert_new_source_array_into_pkgbuild / set_custom_name_in_pkgbuild
//     neither stack source arrays nor accumulate pkgbase suffixes
//     (…-custom stays …-custom)
//   - run_and_remove_testscript does not exec when the script write failed
//   - the makepkg.conf PKGEXT testscript is pinned away from the process CWD

#include "conf-window.cpp"

#include <QApplication>

#include <cstdio>   // for printf
#include <cstdlib>  // for getpid
#include <fstream>  // for ofstream

namespace {

int g_failures = 0;

// Path equality tolerant of symlink components (e.g. /tmp vs /private/tmp
// style aliases); an unresolvable path simply falls back to string equality.
bool same_path(const fs::path& a, const fs::path& b) {
    if (a == b) {
        return true;
    }
    std::error_code ec{};
    return fs::equivalent(a, b, ec);
}

void check(bool condition, const char* what) {
    if (condition) {
        std::printf("PASS: %s\n", what);
    } else {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

fs::path make_tmp_dir(const fs::path& parent, const std::string& name) {
    fs::create_directories(parent / name);
    return parent / name;
}

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream out{path};
    out << content;
}

// A minimal, side-effect-free bash-sourceable PKGBUILD (the linux-cachyos
// AUR shape: PKGBUILD at the repo root).
const std::string kSyntheticPkgbuild =
    "pkgname=foo\n"
    "pkgver=1.0.0\n"
    "pkgrel=1\n"
    "pkgbase=foo\n"
    "source=(\n"
    "\"https://example.com/foo-1.0.0.tar.gz\"\n"
    ")\n"
    "prepare() {\n"
    "\ttrue\n"
    "}\n"
    "package_foo() {\n"
    "\ttrue\n"
    "}\n";

void git_in(const fs::path& dir, const std::string& command) {
    const std::string full = "cd '" + dir.string() + "' && " + command;
    (void)utils::exec(full);
}

std::size_t count_occurrences(const std::string& haystack, const std::string& needle) {
    std::size_t count = 0;
    std::size_t pos   = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

}  // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app{argc, argv};

    const auto root = fs::path{std::string{"/tmp/km-chunk1-test-"} + std::to_string(::getpid())};
    fs::create_directories(root);
    std::printf("test root: %s\n", root.string().c_str());

    // ------------------------------------------------------------------
    // 1. build_repo_path() is absolute (single source of truth for the
    //    flavor paths and the PKGEXT testscript pin).
    // ------------------------------------------------------------------
    check(utils::build_repo_path().is_absolute(), "build_repo_path() is absolute");

    // ------------------------------------------------------------------
    // 2. discover_repo_flavors: single-PKGBUILD layout (AUR linux-cachyos
    //    shape) -> the sole flavor path is the repo root itself.
    // ------------------------------------------------------------------
    const auto single_root = make_tmp_dir(root, "single-repo");
    write_file(single_root / "PKGBUILD", kSyntheticPkgbuild);
    {
        const auto flavors = discover_repo_flavors(single_root, "linux-cachyos");
        check(flavors.size() == 1, "single-package repo yields exactly one flavor");
        if (flavors.size() == 1) {
            check(fs::path{flavors[0].path}.is_absolute(), "single-package flavor path is absolute");
            check(flavors[0].path == single_root.string(), "single-package flavor path is the repo root itself");
            check(flavors[0].key == "cachyos", "single-package flavor key strips the linux- family prefix");
        }
    }

    // ------------------------------------------------------------------
    // 3. discover_repo_flavors: multi-subdir layout -> each flavor path is
    //    repo_root/<subdir> (absolute).
    // ------------------------------------------------------------------
    const auto multi_root = make_tmp_dir(root, "multi-repo");
    write_file(multi_root / "linux-zen" / "PKGBUILD", kSyntheticPkgbuild);
    write_file(multi_root / "linux-zen-rt" / "PKGBUILD", kSyntheticPkgbuild);
    {
        const auto flavors = discover_repo_flavors(multi_root, "multi-repo");
        check(flavors.size() == 2, "multi-subdir repo yields two flavors");
        if (flavors.size() == 2) {
            check(fs::path{flavors[0].path}.is_absolute() && fs::path{flavors[1].path}.is_absolute(), "multi-subdir flavor paths are absolute");
            check(flavors[0].path == (multi_root / "linux-zen").string(), "first flavor path is repo_root/linux-zen");
            check(flavors[1].path == (multi_root / "linux-zen-rt").string(), "second flavor path is repo_root/linux-zen-rt");
            check(flavors[0].key == "zen" && flavors[1].key == "rt", "flavor keys derive from the family prefix");
            check(fs::exists(flavors[0].path) && fs::exists(flavors[1].path), "flavor paths exist on disk");
        }
    }

    // ------------------------------------------------------------------
    // 4. discover_repo_flavors: repo without any PKGBUILD -> empty (no
    //    crash, no phantom flavor).
    // ------------------------------------------------------------------
    {
        const auto empty_root = make_tmp_dir(root, "empty-repo");
        const auto flavors    = discover_repo_flavors(empty_root, "empty-repo");
        check(flavors.empty(), "repo without PKGBUILD yields no flavors");
    }

    // ------------------------------------------------------------------
    // 5. prepare_git_repo (local origin, success): the repo is cloned and
    //    the caller's CWD is restored afterwards.
    // ------------------------------------------------------------------
    const auto origin = make_tmp_dir(root, "origin");
    git_in(origin, "git init -b master");
    write_file(origin / "PKGBUILD", kSyntheticPkgbuild);
    git_in(origin, "git add PKGBUILD && git -c user.name=test -c user.email=test@example.com commit -m init");

    const auto marker = make_tmp_dir(root, "cwd-marker");
    const auto restore_marker = [&marker]() {
        fs::current_path(marker);
    };
    restore_marker();
    {
        // The caller's CWD for prepare_git_repo is the marker dir; the
        // function must leave the process exactly where it found it.
        const auto before = fs::current_path();
        utils::prepare_git_repo(root / "clone-parent", root / "clone-parent/repo", (std::string{"file://"} + origin.string()).c_str());
        const auto after = fs::current_path();
        check(same_path(before, after), "CWD restored after a successful prepare_git_repo");
        check(fs::exists(root / "clone-parent" / "repo" / "PKGBUILD"), "repo was cloned (PKGBUILD present)");
        check(fs::exists(root / "clone-parent" / "repo" / ".git"), "cloned repo has a .git directory");
    }

    // ------------------------------------------------------------------
    // 6. prepare_git_repo (clone failure: nonexistent origin): CWD is
    //    still restored.
    // ------------------------------------------------------------------
    {
        restore_marker();
        const auto before = fs::current_path();
        utils::prepare_git_repo(root / "fail-parent", root / "fail-parent/repo", (std::string{"file://"} + root.string() + "/no-such-origin").c_str());
        const auto after = fs::current_path();
        check(same_path(before, after), "CWD restored after a failed git clone");
        check(!fs::exists(root / "fail-parent" / "repo" / "PKGBUILD"), "failed clone left no repo behind");
    }

    // ------------------------------------------------------------------
    // 7. prepare_git_repo (re-prepare of the existing repo, same origin):
    //    the stale-origin branch runs and the CWD is restored again.
    // ------------------------------------------------------------------
    {
        restore_marker();
        const auto before = fs::current_path();
        utils::prepare_git_repo(root / "clone-parent", root / "clone-parent/repo", (std::string{"file://"} + origin.string()).c_str());
        const auto after = fs::current_path();
        check(same_path(before, after), "CWD restored after re-preparing an existing repo");
        check(fs::exists(root / "clone-parent" / "repo" / "PKGBUILD"), "existing repo still intact after re-prepare");
    }

    // ------------------------------------------------------------------
    // 8. run_and_remove_testscript: write failure -> no exec, empty result
    //    (the pre-fix code exec'd anyway and returned popen's "-1" noise).
    // ------------------------------------------------------------------
    {
        const auto result = run_and_remove_testscript((root / "no-such-dir" / ".testscript").string(), "#!/usr/bin/bash\necho hello\n");
        check(result.empty(), "testscript in a nonexistent dir is not executed (empty result, no '-1')");
    }

    // ------------------------------------------------------------------
    // 9. run_and_remove_testscript: success path runs the script and
    //    removes the file.
    // ------------------------------------------------------------------
    {
        const auto dir    = make_tmp_dir(root, "ts-success");
        const auto result = run_and_remove_testscript((dir / ".testscript").string(), "#!/usr/bin/bash\necho hello\n");
        check(result == "hello", "testscript executes and its output is captured");
        check(!fs::exists(dir / ".testscript"), "testscript file is removed afterwards");
    }

    // ------------------------------------------------------------------
    // 10. get_source_array_from_pkgbuild with an ABSOLUTE flavor path: the
    //     .testscript lands inside the existing flavor dir (the exact
    //     linux-cachyos crash scenario, which used to ENOENT on
    //     <repo>/linux-cachyos/.testscript).
    // ------------------------------------------------------------------
    {
        const auto dir       = make_tmp_dir(root, "srcarray");
        write_file(dir / "PKGBUILD", kSyntheticPkgbuild);
        const auto src_array = get_source_array_from_pkgbuild(dir.string(), "per_gov=yes\n");
        check(src_array.size() == 1 && src_array[0] == "https://example.com/foo-1.0.0.tar.gz", "source array read back from the PKGBUILD via an absolute path");
        check(!fs::exists(dir / ".testscript"), "source-array testscript removed afterwards");
    }

    // ------------------------------------------------------------------
    // 11. insert_new_source_array_into_pkgbuild: missing PKGBUILD -> false
    //     (no write_to_file on empty content, the pre-fix hard-failure
    //     path).
    // ------------------------------------------------------------------
    {
        const auto dir = make_tmp_dir(root, "insert-missing");
        QListWidget list;
        const bool ok = insert_new_source_array_into_pkgbuild(dir.string(), &list, {"https://example.com/foo-1.0.0.tar.gz"});
        check(!ok, "missing PKGBUILD is a clean failure (false)");
        check(!fs::exists(dir / "PKGBUILD"), "missing PKGBUILD was not created by a stray write");
    }

    // ------------------------------------------------------------------
    // 12. insert_new_source_array_into_pkgbuild: absolute flavor path +
    //     list-widget item -> the source array is inserted before
    //     prepare() and the file keeps its structure.
    // ------------------------------------------------------------------
    {
        const auto dir = make_tmp_dir(root, "insert-ok");
        write_file(dir / "PKGBUILD", kSyntheticPkgbuild);
        QListWidget list;
        list.addItem("mylocal.patch");
        const bool ok = insert_new_source_array_into_pkgbuild(dir.string(), &list, {"https://example.com/foo-1.0.0.tar.gz"});
        check(ok, "source array inserted into the PKGBUILD (absolute path)");
        const auto content = utils::read_whole_file((dir / "PKGBUILD").string());
        check(content.find("mylocal.patch") != std::string::npos, "list-widget patch entry present in the rewritten PKGBUILD");
        check(content.find("prepare()") != std::string::npos, "prepare() function still present after the insert");
    }

    // ------------------------------------------------------------------
    // 13. set_custom_name_in_pkgbuild: absolute flavor path rewrites the
    //     single canonical pkgbase= line.
    // ------------------------------------------------------------------
    {
        const auto dir = make_tmp_dir(root, "custom-name");
        write_file(dir / "PKGBUILD", kSyntheticPkgbuild);
        const bool ok = set_custom_name_in_pkgbuild(dir.string(), "foo-custom");
        check(ok, "custom name applied (absolute path)");
        const auto content = utils::read_whole_file((dir / "PKGBUILD").string());
        check(content.find("pkgbase=\"foo-custom\"") != std::string::npos, "pkgbase= rewritten to the custom name");
    }

    // ------------------------------------------------------------------
    // 14. get_package_names_glob_from_pkgbuild: the makepkg.conf PKGEXT
    //     testscript is pinned to the build repo root, NOT dropped in the
    //     process CWD (here the marker dir) — and the glob is produced.
    // ------------------------------------------------------------------
    {
        const auto dir    = make_tmp_dir(root, "glob");
        write_file(dir / "PKGBUILD", kSyntheticPkgbuild);
        const auto before = fs::current_path();
        fs::current_path(marker);
        const auto globs = get_package_names_glob_from_pkgbuild(dir.string());
        fs::current_path(before);
        check(globs.size() == 1 && globs.front().starts_with("foo-1.0.0-1-"), "package name glob produced from the PKGBUILD");
        check(!fs::exists(marker / ".testscriptpkgext"), "PKGEXT testscript was NOT written into the process CWD");
        check(!fs::exists(dir / ".testscriptpkgnames"), "pkgnames testscript removed afterwards");
    }

    // ------------------------------------------------------------------
    // 15. insert_new_source_array_into_pkgbuild is IDEMPOTENT: a second
    //     run replaces the block the first run inserted instead of
    //     stacking another (the pre-fix accumulation: the user's build
    //     dir ended up with two duplicated source= arrays).
    // ------------------------------------------------------------------
    {
        const auto dir = make_tmp_dir(root, "insert-twice");
        write_file(dir / "PKGBUILD", kSyntheticPkgbuild);
        QListWidget list;
        list.addItem("mylocal.patch");
        check(insert_new_source_array_into_pkgbuild(dir.string(), &list, {"https://example.com/foo-1.0.0.tar.gz"}), "first source-array insert succeeds");
        const auto after_first = utils::read_whole_file((dir / "PKGBUILD").string());
        check(count_occurrences(after_first, "source=(") == 1, "exactly one source= array after the first insert (original replaced in place)");
        check(after_first.find("https://example.com/foo-1.0.0.tar.gz") != std::string::npos, "original source entry preserved in the rewritten array");
        QListWidget list2;
        list2.addItem("mylocal.patch");
        check(insert_new_source_array_into_pkgbuild(dir.string(), &list2, {"https://example.com/foo-1.0.0.tar.gz"}), "second source-array insert succeeds");
        const auto after_second = utils::read_whole_file((dir / "PKGBUILD").string());
        check(count_occurrences(after_second, "source=(") == 1, "still exactly one source= array after the second insert (no stacking)");
        check(after_second == after_first, "second run is byte-stable against the first (idempotent)");
    }

    // ------------------------------------------------------------------
    // 16. set_custom_name_in_pkgbuild is IDEMPOTENT for a "$pkgbase"
    //     placeholder name: a repeat run must not re-wrap the already
    //     renamed pkgbase (the pre-fix accumulation: the user's build
    //     dir ended up with pkgbase="…-custom-custom").
    // ------------------------------------------------------------------
    {
        const auto dir = make_tmp_dir(root, "custom-name-twice");
        write_file(dir / "PKGBUILD", kSyntheticPkgbuild);
        check(set_custom_name_in_pkgbuild(dir.string(), "$pkgbase-custom"), "first custom-name run succeeds");
        auto content = utils::read_whole_file((dir / "PKGBUILD").string());
        check(content.find("pkgbase=\"foo-custom\"") != std::string::npos, "pkgbase= rewritten to foo-custom");
        check(set_custom_name_in_pkgbuild(dir.string(), "$pkgbase-custom"), "second custom-name run succeeds");
        content = utils::read_whole_file((dir / "PKGBUILD").string());
        check(content.find("pkgbase=\"foo-custom\"") != std::string::npos, "repeat run lands on the same value");
        check(content.find("custom-custom") == std::string::npos, "no accumulated suffix after the repeat run");
    }

    // ------------------------------------------------------------------
    // 17. A pkgbase already carrying a DOUBLED suffix (the user's live
    //     state: pkgbase="linux-$_pkgsuffix-custom-custom") collapses to a
    //     single suffix on the next run and stays there (fixed point).
    // ------------------------------------------------------------------
    {
        const auto dir = make_tmp_dir(root, "custom-name-double");
        std::string pkgbuild = kSyntheticPkgbuild;
        (void)utils::replace_all(pkgbuild, "pkgbase=foo", "pkgbase=\"linux-$_pkgsuffix-custom-custom\"");
        write_file(dir / "PKGBUILD", pkgbuild);
        check(set_custom_name_in_pkgbuild(dir.string(), "$pkgbase-custom"), "run on the already-doubled state succeeds");
        auto content = utils::read_whole_file((dir / "PKGBUILD").string());
        check(content.find("pkgbase=\"linux-$_pkgsuffix-custom\"") != std::string::npos, "doubled suffix collapsed to a single one");
        check(set_custom_name_in_pkgbuild(dir.string(), "$pkgbase-custom"), "follow-up run succeeds");
        content = utils::read_whole_file((dir / "PKGBUILD").string());
        check(content.find("custom-custom") == std::string::npos, "the value no longer grows (stable fixed point)");
    }

    // ------------------------------------------------------------------
    // 18. AUR layout (the user's real file): the original source= array
    //     is separated from prepare() by other top-level statements
    //     (validpgpkeys=) + a blank line — it must stay untouched, while
    //     the block inserted above prepare() is replaced in place on
    //     repeat runs.
    // ------------------------------------------------------------------
    {
        const std::string kAurShape =
            "pkgname=foo\n"
            "pkgver=1.0.0\n"
            "pkgbase=foo\n"
            "source=(\n"
            "\"https://example.com/foo.tar.gz\"\n"
            ")\n"
            "validpgpkeys=(\n"
            "AB12CD34\n"
            ")\n"
            "\n"
            "prepare() {\n"
            "\ttrue\n"
            "}\n";
        const auto dir = make_tmp_dir(root, "aur-shape");
        write_file(dir / "PKGBUILD", kAurShape);
        QListWidget list;
        list.addItem("p1.patch");
        check(insert_new_source_array_into_pkgbuild(dir.string(), &list, {"https://example.com/foo.tar.gz"}), "insert into the AUR layout succeeds");
        auto content = utils::read_whole_file((dir / "PKGBUILD").string());
        check(count_occurrences(content, "source=(") == 2, "original source= block untouched + exactly one inserted block");
        check(content.find("validpgpkeys=(") != std::string::npos, "validpgpkeys= section untouched");
        check(content.find("p1.patch") != std::string::npos, "widget entry present in the inserted block");
        QListWidget list2;
        list2.addItem("p1.patch");
        check(insert_new_source_array_into_pkgbuild(dir.string(), &list2, {"https://example.com/foo.tar.gz"}), "second insert into the AUR layout succeeds");
        content = utils::read_whole_file((dir / "PKGBUILD").string());
        check(count_occurrences(content, "source=(") == 2, "second run replaces the inserted block (no stacking)");
        check(content.find("validpgpkeys=(") != std::string::npos, "validpgpkeys= section still untouched");
    }

    // ------------------------------------------------------------------
    fs::remove_all(root);
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
