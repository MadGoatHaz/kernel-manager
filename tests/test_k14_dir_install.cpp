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

// Standalone unit test for the "install from a directory" capability
// (Feature A, D2 — the permanent k14 harness). k7-recipe: compiles the
// real install_kernel module plus the modules it reuses with the
// project's GCC warning set (no CTest infra in this project; no event
// loop and no offscreen platform — the API is Qt-free). Asserts:
//   - fixture build (before any check): an mkstemp sandbox + two REAL
//     built packages (linux-test + linux-test-headers, .PKGINFO + a
//     payload, `tar --zstd -cf` — the verified tooling, D2) — each
//     fixture build asserts rc == 0, so a missing tar/zstd fails the
//     harness loudly, never vacuously
//   - (A) list_local_packages: empty dir ⇒ 0; the fixture dir ⇒
//     exactly the 2 zst packages, sorted (kernel before headers by
//     filename); decoys (foo.txt + a non-zst .pkg.tar.gz) ignored; a
//     subdirectory holding packages NOT listed (non-recursive, D2)
//   - (B) read_pkginfo: kernel pkg ⇒ linux-test / 1.2.3-4; headers pkg
//     ⇒ linux-test-headers / 1.2.3-4; a non-archive .pkg.tar.zst
//     (plain text) ⇒ false, no crash; a package with a missing pkgrel
//     line ⇒ version = the bare pkgver (lenient parse)
//   - (C) install_from_directory with a fake recording CommandRunner
//     (k8 pattern — the real runCmdTerminal is never resolved, no
//     terminal, pkexec, pacman or network is touched): fixture dir +
//     GRUB ⇒ exactly 2 calls ([0] the exact `pacman -U '<abs1>'
//     '<abs2>'`, [1] the single consolidated postinstall_tail.sh step
//     (chunks A/B/C) carrying the resolved .PKGINFO name + version +
//     the tail's bootloader name, all escalated) + ok + verdict ==
//     BOOT_SAFE + identity linux-test / 1.2.3-4 + boot instructions
//     filled; UNKNOWN ⇒ 2 calls (install + the single tail); empty dir
//     ⇒ the exact "no packages" error, 0 calls, no instructions; fake
//     rc = 1 on step 0 (the phase-a pacman -U) ⇒ ok = false, verdict ==
//     INSTALLATION_FAILED, the error names the pacman step + rc,
//     exactly 1 call (the tail is skipped), instructions still filled;
//     the D-E verdict probe (per-call rcs): phase-a rc 0 + tail rc 1
//     ⇒ INSTALLED_NOT_BOOT_SAFE (never a false SUCCESS).

#include "install_kernel.hpp"

#include <cstdio>      // for printf
#include <cstdlib>     // for system, mkstemp
#include <filesystem>  // for path, create_directories, copy_file, remove_all
#include <fstream>     // for ofstream
#include <string>      // for string
#include <system_error> // for error_code
#include <utility>     // for pair
#include <vector>      // for vector

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

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// The mandatory closing note instructions_for (K6) appends for every
// bootloader: one constant text (same text the k8 harness asserts).
constexpr const char* expected_note =
    "The kernel is installed and ready to boot. Select it from your bootloader menu at next reboot.";

// The single consolidated tail step append_postinstall_tail emits (chunk
// E; the exact command the .cpp pushes, kept in lockstep with the
// implementation): ONE postinstall_tail.sh call (chunks A/B/C) carrying
// (pkgname, kver, bootloader). The dir flow passes the resolved .PKGINFO
// name + version (so the tail targets the new <KVER> directly) and the
// tail's bootloader name (chunk C's lowercase systemd-boot/grub/uki/
// unknown contract — NOT the UI-facing bootloader_name() strings). The
// single quotes mirror the .cpp's shell_quote for these quote-free values.
std::string tail_cmd(const std::string& pkg, const std::string& kver, Bootloader bl) {
    const char* blname = "unknown";
    switch (bl) {
        case Bootloader::SYSTEMD_BOOT: blname = "systemd-boot"; break;
        case Bootloader::GRUB:         blname = "grub"; break;
        case Bootloader::UKI:          blname = "uki"; break;
        case Bootloader::UNKNOWN:      blname = "unknown"; break;
    }
    return std::string(KM_HELPER_DIR) + "/postinstall_tail.sh '" + pkg + "' '" + kver + "' '" + blname + "'";
}

// A recording command runner (k8 pattern): returns a fixed exit code
// for every step and records the (cmd, escalate) pairs asked to run,
// so no real terminal, pkexec, pacman or network is ever touched. The
// lambda captures the struct by POINTER (a std::function copy would
// record into its own copy of the lambda's captures — the k7/C7
// reference-capture idiom).
struct RecordingRunner {
    int rc = 0;
    std::vector<std::pair<std::string, bool>> calls{};
    CommandRunner fn = [this](const std::string& cmd, bool escalate) noexcept -> int {
        calls.emplace_back(cmd, escalate);
        return rc;
    };
};

// A per-call scripted runner (the D-E 3-state verdict probe): returns a
// distinct exit code for the Nth call (rcs[0] for call 1, rcs[1] for
// call 2, ...; 0 past the end) and records the (cmd, escalate) pairs, so
// a phase-a rc 0 + tail rc 1 can be driven to INSTALLED_NOT_BOOT_SAFE
// (no real terminal, pkexec, pacman or network is touched).
struct ScriptedRunner {
    std::vector<int> rcs{};
    std::vector<std::pair<std::string, bool>> calls{};
    CommandRunner fn = [this](const std::string& cmd, bool escalate) noexcept -> int {
        calls.emplace_back(cmd, escalate);
        const std::size_t idx = calls.size() - 1;
        return idx < rcs.size() ? rcs[idx] : 0;
    };
};

// Write a plain-text file (false on any failure).
bool write_text(const std::filesystem::path& file, const std::string& content) {
    std::ofstream out{file};
    if (!out) {
        return false;
    }
    out << content;
    return out.good();
}

// Build one REAL *.pkg.tar.zst package: write the .PKGINFO (+ one dummy
// payload file) into the staging dir, then `tar --zstd -cf` the two
// members (the members are stored as bare names — `.PKGINFO` is
// exactly the member read_pkginfo extracts, the archive root layout).
// The build asserts rc == 0: a missing tar/zstd ⇒ the harness fails
// loudly, never vacuously (the D2/C4 spec).
bool build_package(const std::filesystem::path& pkg,
                   const std::filesystem::path& staging,
                   const std::string& pkginfo) {
    if (!write_text(staging / ".PKGINFO", pkginfo)
        || !write_text(staging / "payload.txt", "dummy payload\n")) {
        check(false, ("fixture: staging files for " + pkg.filename().string()).c_str());
        return false;
    }
    const std::string cmd = "tar --zstd -cf '" + pkg.string()
                            + "' -C '" + staging.string()
                            + "' .PKGINFO payload.txt 2>/dev/null";
    const int rc = std::system(cmd.c_str());
    check(rc == 0, ("fixture: `tar --zstd -cf` rc==0 for " + pkg.filename().string()).c_str());
    return rc == 0;
}

}  // namespace

int main() {
    // ------------------------------------------------------------------
    // 0. Fixture build (before any check): the mkstemp sandbox + the
    //    real packages. Layout:
    //      <sandbox>/pkgs/  — the fixture dir: the 2 real zst packages
    //                         + decoys (foo.txt, legacy.pkg.tar.gz) +
    //                         a sub/ dir holding a copy of a package
    //      <sandbox>/empty/ — an empty dir
    //      <sandbox>/bad/   — a non-archive .pkg.tar.zst (plain text)
    //      <sandbox>/norel/ — a real package whose .PKGINFO lacks pkgrel
    //      <sandbox>/stage/ — per-package tar staging dirs
    // ------------------------------------------------------------------
    char tmpl[] = "/tmp/km14-sandbox.XXXXXX";
    char* const mkdtemp_result = mkdtemp(tmpl);
    check(mkdtemp_result != nullptr, "fixture: sandbox created (mkdtemp)");
    if (mkdtemp_result == nullptr) {
        std::printf("%d TEST(S) FAILED\n", g_failures);
        return 1;
    }
    const std::filesystem::path sandbox = tmpl;
    // The API takes std::string_view (path has no one-step conversion to
    // it): the dir strings for the calls under test.
    const std::string pkgs_dir = (sandbox / "pkgs").string();
    const std::string empty_dir = (sandbox / "empty").string();

    std::error_code ec{};
    const bool dirs_ok = std::filesystem::create_directories(sandbox / "pkgs" / "sub", ec)
                         && std::filesystem::create_directories(sandbox / "empty", ec)
                         && std::filesystem::create_directories(sandbox / "bad", ec)
                         && std::filesystem::create_directories(sandbox / "norel", ec)
                         && std::filesystem::create_directories(sandbox / "stage" / "linux-test", ec)
                         && std::filesystem::create_directories(sandbox / "stage" / "linux-test-headers", ec)
                         && std::filesystem::create_directories(sandbox / "stage" / "linux-norel", ec);
    check(dirs_ok, "fixture: sandbox subdirs created");

    // The two real packages of the fixture dir (+ their staging dirs).
    const std::filesystem::path kernel_pkg = sandbox / "pkgs" / "linux-test-1.2.3-4-x86_64.pkg.tar.zst";
    const std::filesystem::path headers_pkg = sandbox / "pkgs" / "linux-test-headers-1.2.3-4-x86_64.pkg.tar.zst";
    const bool kernel_built = build_package(
        kernel_pkg,
        sandbox / "stage" / "linux-test",
        "pkgname = linux-test\npkgbase = linux-test\npkgarch = x86_64\npkgver = 1.2.3\npkgrel = 4\n");
    const bool headers_built = build_package(
        headers_pkg,
        sandbox / "stage" / "linux-test-headers",
        "pkgname = linux-test-headers\npkgbase = linux-test\npkgarch = x86_64\npkgver = 1.2.3\npkgrel = 4\n");
    check(kernel_built && headers_built, "fixture: the 2 real packages built");

    // Decoys in the fixture dir: a plain text file, a non-zst package,
    // and a SUBDIRECTORY holding a copy of a package (the non-recursive
    // gate — none of these may be listed).
    check(write_text(sandbox / "pkgs" / "foo.txt", "decoy text\n"), "fixture: decoy foo.txt written");
    check(write_text(sandbox / "pkgs" / "legacy.pkg.tar.gz", "decoy, not zstd\n"), "fixture: decoy .pkg.tar.gz written");
    ec.clear();
    std::filesystem::copy_file(kernel_pkg, sandbox / "pkgs" / "sub" / "nested.pkg.tar.zst", ec);
    check(!ec, "fixture: package copied into the sub/ directory");

    // The bad dir: a .pkg.tar.zst NAME holding plain text (not a zstd
    // archive) — read_pkginfo must degrade to false, never crash.
    check(write_text(sandbox / "bad" / "fake.pkg.tar.zst", "this is not a zstd archive\n"),
          "fixture: non-archive fake.pkg.tar.zst written");

    // The norel dir: a real package whose .PKGINFO has no pkgrel line
    // (the lenient-parse gate — version degrades to the bare pkgver).
    const std::filesystem::path norel_pkg = sandbox / "norel" / "linux-norel-1.2.3-x86_64.pkg.tar.zst";
    const bool norel_built = build_package(
        norel_pkg,
        sandbox / "stage" / "linux-norel",
        "pkgname = linux-norel\npkgbase = linux-norel\npkgarch = x86_64\npkgver = 1.2.3\n");
    check(norel_built, "fixture: the no-pkgrel package built");

    // ------------------------------------------------------------------
    // (A) list_local_packages (non-recursive, .pkg.tar.zst, sorted).
    // ------------------------------------------------------------------
    {
        const auto empty_pkgs = list_local_packages(empty_dir);
        check(empty_pkgs.empty(), "A: empty dir ⇒ 0 packages");

        const auto pkgs = list_local_packages(pkgs_dir);
        check(pkgs.size() == 2, "A: fixture dir ⇒ exactly 2 packages listed");
        if (pkgs.size() == 2) {
            check(pkgs[0] == kernel_pkg && pkgs[1] == headers_pkg,
                  "A: sorted by filename (kernel before headers)");
            check(!pkgs[0].parent_path().filename().string().starts_with("sub")
                  && !pkgs[1].parent_path().filename().string().starts_with("sub"),
                  "A: the sub/ directory's package is NOT listed (non-recursive)");
            check(pkgs[0].filename().string().ends_with(".pkg.tar.zst")
                  && pkgs[1].filename().string().ends_with(".pkg.tar.zst"),
                  "A: the decoys (foo.txt + .pkg.tar.gz) are NOT listed");
        }
    }

    // ------------------------------------------------------------------
    // (B) read_pkginfo (.PKGINFO identity out of the real archives).
    // ------------------------------------------------------------------
    {
        std::string name{};
        std::string version{};
        check(read_pkginfo(kernel_pkg, name, version) && name == "linux-test" && version == "1.2.3-4",
              "B: kernel pkg ⇒ linux-test / 1.2.3-4");

        name.clear();
        version.clear();
        check(read_pkginfo(headers_pkg, name, version) && name == "linux-test-headers" && version == "1.2.3-4",
              "B: headers pkg ⇒ linux-test-headers / 1.2.3-4");

        name.clear();
        version.clear();
        check(!read_pkginfo(sandbox / "bad" / "fake.pkg.tar.zst", name, version),
              "B: non-archive .pkg.tar.zst ⇒ false (no crash)");

        name.clear();
        version.clear();
        check(read_pkginfo(norel_pkg, name, version) && name == "linux-norel" && version == "1.2.3",
              "B: missing pkgrel ⇒ version = the bare pkgver (lenient parse)");
    }

    // ------------------------------------------------------------------
    // (C) install_from_directory (the full C2 contract, fake recording
    //     runner — nothing real is executed).
    // ------------------------------------------------------------------
    {
        // Fixture dir + GRUB: the exact 2-call sequence (pacman -U + the
        // single consolidated tail) + identity + verdict + the filled boot
        // instructions.
        RecordingRunner runner{};
        const DirInstallResult grub = install_from_directory(pkgs_dir, runner.fn, Bootloader::GRUB);
        check(grub.ok && grub.error.empty(), "C: fixture dir + GRUB ⇒ ok (BOOT_SAFE), no error");
        check(grub.verdict == PostinstallVerdict::BOOT_SAFE, "C: GRUB ⇒ verdict == BOOT_SAFE");
        check(runner.calls.size() == 2, "C: GRUB ⇒ exactly 2 calls (pacman -U + the single consolidated tail)");
        if (runner.calls.size() == 2) {
            check(runner.calls[0].first == "pacman -U '" + kernel_pkg.string() + "' '" + headers_pkg.string() + "'"
                  && runner.calls[0].second,
                  "C[0]: the exact escalated `pacman -U '<abs1>' '<abs2>'` (quoted absolutes, no glob)");
            check(runner.calls[1].first == tail_cmd("linux-test", "1.2.3-4", Bootloader::GRUB) && runner.calls[1].second,
                  "C[1]: the single consolidated tail (escalated; the .PKGINFO name + version + the GRUB tail name)");
        }
        check(grub.name == "linux-test" && grub.version == "1.2.3-4",
              "C: identity = the kernel package (linux-test / 1.2.3-4)");
        check(grub.boot_instructions.size() == 4 && grub.boot_instructions.back() == expected_note,
              "C: GRUB boot instructions filled (3 steps + the note, ends with the note)");

        // Fixture dir + UNKNOWN: 2 calls (the single tail carries the
        // bootloader — no separate GRUB refresh).
        RecordingRunner unknown_runner{};
        const DirInstallResult unknown = install_from_directory(pkgs_dir, unknown_runner.fn, Bootloader::UNKNOWN);
        check(unknown.ok && unknown.verdict == PostinstallVerdict::BOOT_SAFE, "C: fixture dir + UNKNOWN ⇒ ok (BOOT_SAFE)");
        check(unknown_runner.calls.size() == 2, "C: UNKNOWN ⇒ 2 calls (install + the single tail, no separate grub-mkconfig)");
        if (unknown_runner.calls.size() == 2) {
            check(unknown_runner.calls[0].first == "pacman -U '" + kernel_pkg.string() + "' '" + headers_pkg.string() + "'"
                  && unknown_runner.calls[0].second,
                  "C[0]: the exact escalated `pacman -U '<abs1>' '<abs2>'`");
            check(unknown_runner.calls[1].first == tail_cmd("linux-test", "1.2.3-4", Bootloader::UNKNOWN) && unknown_runner.calls[1].second,
                  "C[1]: the single consolidated tail (escalated, bootloader = the tail's 'unknown')");
        }
        check(unknown.boot_instructions.size() == 3 && unknown.boot_instructions.back() == expected_note,
              "C: UNKNOWN boot instructions filled (2 steps + the note)");

        // Empty dir: the exact error, zero runner calls (the D2 guard),
        // no boot instructions (no kernel to point at).
        RecordingRunner empty_runner{};
        const DirInstallResult empty = install_from_directory(empty_dir, empty_runner.fn, Bootloader::GRUB);
        check(!empty.ok, "C: empty dir ⇒ not ok");
        check(empty.error == "no *.pkg.tar.zst packages found in '" + (sandbox / "empty").string() + "'",
              "C: empty dir ⇒ the exact 'no packages' error");
        check(empty_runner.calls.empty(), "C: empty dir ⇒ 0 runner calls");
        check(empty.boot_instructions.empty(), "C: empty dir ⇒ no boot instructions");

        // Fake rc = 1 on step 0 (the phase-a pacman -U): graceful stop —
        // verdict == INSTALLATION_FAILED, the error names the pacman step
        // + its rc, exactly 1 call (the tail is skipped), instructions
        // still filled (the install_kernel pattern).
        RecordingRunner fail_runner{};
        fail_runner.rc = 1;
        const DirInstallResult fail = install_from_directory(pkgs_dir, fail_runner.fn, Bootloader::GRUB);
        check(!fail.ok, "C: fake rc=1 on the phase-a pacman -U ⇒ not ok");
        check(fail.verdict == PostinstallVerdict::INSTALLATION_FAILED, "C: phase-a rc=1 ⇒ verdict == INSTALLATION_FAILED");
        check(fail.error == "Command failed (exit code 1): pacman -U '" + kernel_pkg.string() + "' '" + headers_pkg.string() + "'",
              "C: the error names the pacman step + rc 1");
        check(fail_runner.calls.size() == 1, "C: exactly 1 call (the post-install tail is skipped)");
        check(fail.boot_instructions.size() == 4 && fail.boot_instructions.back() == expected_note,
              "C: boot instructions still filled on failure");

        // The D-E 3-state verdict probe (per-call rcs — the phase-a rc and
        // the tail rc judged independently; the H3 false-SUCCESS is gone):
        // phase-a rc 0 + tail rc 1 ⇒ INSTALLED_NOT_BOOT_SAFE (ok false);
        // phase-a rc 0 + tail rc 0 ⇒ BOOT_SAFE (ok true).
        ScriptedRunner nbs{};
        nbs.rcs = {0, 1};
        const DirInstallResult r_nbs = install_from_directory(pkgs_dir, nbs.fn, Bootloader::GRUB);
        check(!r_nbs.ok && r_nbs.verdict == PostinstallVerdict::INSTALLED_NOT_BOOT_SAFE,
              "C: tail rc=1 ⇒ INSTALLED_NOT_BOOT_SAFE (not a false SUCCESS)");
        check(nbs.calls.size() == 2, "C: tail rc=1 ⇒ exactly 2 calls (phase-a ran, then the tail)");
        check(contains(r_nbs.error, "not boot-safe"), "C: tail rc=1 ⇒ the error names the tail gap");

        ScriptedRunner bs{};
        bs.rcs = {0, 0};
        const DirInstallResult r_bs = install_from_directory(pkgs_dir, bs.fn, Bootloader::GRUB);
        check(r_bs.ok && r_bs.verdict == PostinstallVerdict::BOOT_SAFE,
              "C: phase-a rc=0 + tail rc=0 ⇒ BOOT_SAFE (ok)");
    }

    // ------------------------------------------------------------------
    // Sandbox cleanup: the harness must not leave anything in /tmp.
    // ------------------------------------------------------------------
    ec.clear();
    std::filesystem::remove_all(sandbox, ec);
    check(!ec, "sandbox removed");

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}
