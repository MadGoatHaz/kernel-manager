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

#include "kernel_info.hpp"

#include <algorithm>      // for ranges::replace
#include <array>          // for array
#include <cctype>         // for tolower, isalnum, isdigit, isspace
#include <cstdio>         // for popen, pclose, fread
#include <filesystem>     // for path
#include <fstream>        // for ifstream
#include <iterator>       // for istreambuf_iterator
#include <mutex>          // for mutex, lock_guard
#include <unordered_map>  // for unordered_map

namespace kernel_info {
namespace {

    // The output caps for the single popen helper (D5 risk 2): kconfig and the
    // uname/sysctl/lsmod/modinfo captures are small (well under 1 MiB); a
    // module disassembly can be multi-MB, so it gets the 2 MiB cap (the .text
    // section sits at the file head, so the cap still captures the instructions
    // we scan for).
    constexpr std::size_t kDefaultCap   = 1u << 20u;  // 1 MiB
    constexpr std::size_t kDisasmCap    = 2u << 20u;  // 2 MiB
    constexpr std::size_t kSampleBudget = 8;          // max disassemble dispatches per process

    // Lowercased copy (the compiler tokens are matched case-insensitively —
    // the driver_gate lower() precedent).
    [[nodiscard]] std::string lower(std::string_view value) {
        std::string out{};
        out.reserve(value.size());
        for (const char c : value) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return out;
    }

    // Trim leading + trailing whitespace/CR (sysfs attribute files and shell
    // captures carry a trailing newline; the values are a handful of tokens).
    [[nodiscard]] std::string_view trim(std::string_view value) {
        std::size_t start = 0;
        while (start < value.size() && std::isspace(static_cast<unsigned char>(value.at(start)))) {
            ++start;
        }
        std::size_t end = value.size();
        while (end > start && std::isspace(static_cast<unsigned char>(value.at(end - 1)))) {
            --end;
        }
        return value.substr(start, end - start);
    }

    // Read a small file's whole content ("" when absent/unreadable — the /proc
    // and /sys files here are a handful of bytes to ~70 KB; nothing streams).
    // The ifstream open degrades silently on EACCES (the sched/preempt file is
    // root-only for a non-root app — D5 risk 4), so no error noise is printed.
    [[nodiscard]] std::string read_small_file(const std::filesystem::path& path) {
        std::ifstream in{path};
        if (!in) {
            return {};
        }
        return {(std::istreambuf_iterator<char>{in}), std::istreambuf_iterator<char>{}};
    }

    // The module's single popen helper (the driver_gate run_fixed_command
    // shape): runs one command string and captures its stdout, stopping at
    // `max_bytes`. The command strings are fixed literals or character-class-
    // guarded names built at the call site (never raw user text). On popen
    // failure the sentinel "-1" is returned and callers treat it as "no
    // signal" (the degradation contract), never as data.
    [[nodiscard]] std::string run_command(std::string_view command, std::size_t max_bytes = kDefaultCap) {
        // NOLINTNEXTLINE(bugprone-command-processor) — by design: a shell is required to pipe the fixed tool output (zcat / sysctl -n / lsmod / modinfo / llvm-objdump); the command strings are compile-time literals or character-class-guarded names, so no user input reaches the shell. This is the FOURTH standing suppression project-wide (the other three are utils.cpp's popen, bootloader.cpp's `command -v`, and driver_gate.cpp's popen); per CONTRIBUTING it is specific, names the check, and carries its rationale.
        if (FILE* const pipe = popen(std::string{command}.c_str(), "r")) {
            std::string output{};
            char buffer[4096];
            while (output.size() < max_bytes) {
                const std::size_t got = std::fread(buffer, 1, sizeof buffer, pipe);
                if (got == 0) {
                    break;
                }
                const std::size_t room = max_bytes - output.size();
                output.append(buffer, got > room ? room : got);
            }
            pclose(pipe);
            return output;
        }
        return "-1";
    }

    // Character-class guards that keep the parameterized shell strings
    // (sysctl -n <key>, modinfo -F filename <name>, the disassemble paths)
    // metacharacter-free — the driver_gate is_pkg_name_safe precedent. A
    // failing guard degrades to "" (no signal), never a shell injection.
    [[nodiscard]] bool is_name_safe(std::string_view name) {
        // Module names, dotted sysctl keys, and the osrelease: letters,
        // digits, and . _ - (a /proc/modules name is filename-safe).
        if (name.empty() || name.size() > 255) {
            return false;
        }
        for (const char c : name) {
            const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                || c == '.' || c == '_' || c == '-';
            if (!safe) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool is_path_safe(std::string_view path) {
        // A module file path: the name class plus '/' (the disassemble guard).
        if (path.empty() || path.size() > 4096) {
            return false;
        }
        for (const char c : path) {
            const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                || c == '.' || c == '_' || c == '-' || c == '/';
            if (!safe) {
                return false;
            }
        }
        return true;
    }

    // The probe dispatch helpers (the driver_gate ask() no-crash rule): an
    // unbound predicate contributes no signal (""/empty) and never throws.
    [[nodiscard]] std::string ask_str(const std::function<std::string()>& f) {
        return f ? f() : std::string{};
    }

    [[nodiscard]] std::string ask_str_arg(const std::function<std::string(std::string_view)>& f, std::string_view arg) {
        return f ? f(arg) : std::string{};
    }

    [[nodiscard]] std::vector<std::string> ask_vec(const std::function<std::vector<std::string>()>& f) {
        return f ? f() : std::vector<std::string>{};
    }

    // A config line is "KEY=<value>" at the START of a line, so the exact-key
    // tests never cross-match: "CONFIG_PREEMPT=" is not a substring of
    // "CONFIG_PREEMPT_DYNAMIC=", and "CONFIG_HZ=" is not a substring of
    // "CONFIG_HZ_1000=" (the boolean variants).
    [[nodiscard]] bool cfg_has(const std::string& cfg, const std::string& key) {
        const std::string needle = key + "=";
        std::size_t pos          = 0;
        while ((pos = cfg.find(needle, pos)) != std::string::npos) {
            if (pos == 0 || cfg.at(pos - 1) == '\n') {
                return true;
            }
            pos += 1;
        }
        return false;
    }

    // The value of the "KEY=..." line: "y"/"n" verbatim, a quoted value with
    // its quotes stripped, a numeric value verbatim; "" when absent.
    [[nodiscard]] std::string cfg_val(const std::string& cfg, const std::string& key) {
        const std::string needle = key + "=";
        std::size_t pos          = 0;
        while ((pos = cfg.find(needle, pos)) != std::string::npos) {
            if (pos == 0 || cfg.at(pos - 1) == '\n') {
                const std::size_t value_start = pos + needle.size();
                std::size_t value_end         = cfg.find('\n', value_start);
                if (value_end == std::string::npos) {
                    value_end = cfg.size();
                }
                std::string value = cfg.substr(value_start, value_end - value_start);
                if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                    value = value.substr(1, value.size() - 2);
                }
                return value;
            }
            pos += 1;
        }
        return {};
    }

    // The curated x86 CPU-family config symbols (the driver_gate GPU-table
    // shape): an EXACT set, never a bare "CONFIG_M" prefix — that would
    // false-match MEMCG/MMU/MTRR/MITIGATION_*/MODULES/MIGRATION and friends,
    // which sit earlier in a real .config than any family line. Extend as new
    // families ship; a family not listed degrades to the Generic/Custom row
    // (conservative — never a false positive).
    constexpr std::array<std::string_view, 37> kCpuFamilies{
        "CONFIG_M386",
        "CONFIG_M486",
        "CONFIG_M586",
        "CONFIG_M6",
        "CONFIG_MC6X86",
        "CONFIG_MPENTIUM",
        "CONFIG_MPENTIUM_MMMX",
        "CONFIG_MPENTIUMII",
        "CONFIG_MPENTIUMIII",
        "CONFIG_MPENTIUMM",
        "CONFIG_MPENTIUM4",
        "CONFIG_MK6",
        "CONFIG_MK7",
        "CONFIG_MK8",
        "CONFIG_MK10",
        "CONFIG_MNEHALEM",
        "CONFIG_MSILVERMONT",
        "CONFIG_MSANDYBRIDGE",
        "CONFIG_MCORE",
        "CONFIG_MCORE2",
        "CONFIG_MBOBCAT",
        "CONFIG_MJAGUAR",
        "CONFIG_MBDW",
        "CONFIG_MBREED",
        "CONFIG_MBARCELONA",
        "CONFIG_MBOLTON",
        "CONFIG_MZEN",
        "CONFIG_MZEN2",
        "CONFIG_MZEN3",
        "CONFIG_MZEN4",
        "CONFIG_MHASWELL",
        "CONFIG_MBROADWELL",
        "CONFIG_MSKYLAKE",
        "CONFIG_MSKYLAKE_X",
        "CONFIG_MCABYLAKE",
        "CONFIG_MROCKETLAKE",
        "CONFIG_MSAPPHIRERAPIDS",
    };

    // The selected CPU family: the first "CONFIG_M<...>=y" line whose key is
    // one of the curated family symbols; "" when none (the native/generic/
    // custom rows are decided by the caller).
    [[nodiscard]] std::string first_cpu_family(const std::string& cfg) {
        std::size_t pos = 0;
        while (pos <= cfg.size()) {
            const std::size_t end       = cfg.find('\n', pos);
            const std::string_view line = (end == std::string::npos) ? std::string_view{cfg}.substr(pos)
                                                                     : std::string_view{cfg}.substr(pos, end - pos);
            if (line.size() > 2 && line.ends_with("=y")) {
                const std::string_view key = line.substr(0, line.size() - 2);
                for (const std::string_view family : kCpuFamilies) {
                    if (key == family) {
                        return std::string{family};
                    }
                }
            }
            if (end == std::string::npos) {
                break;
            }
            pos = end + 1;
        }
        return {};
    }

    // "x86-64-v<N>" from the selected ISA level, or "". Both naming
    // generations are covered (pre-5.17 CONFIG_X86_64_VERSION_<X>, 5.17+
    // CONFIG_X86_64_V<X>); a non-baseline level beats the baseline (V1 /
    // VERSION_NONE => "x86-64-v1"); no level selected => "".
    [[nodiscard]] std::string detect_isa_level(const std::string& cfg) {
        constexpr std::string_view kOldPrefix = "CONFIG_X86_64_VERSION_";
        constexpr std::string_view kNewPrefix = "CONFIG_X86_64_V";
        std::string non_baseline{};
        bool baseline        = false;
        const auto normalize = [](std::string_view x) {
            std::string out{x};
            std::ranges::replace(out, '_', '.');
            return out;
        };
        std::size_t pos = 0;
        while (pos <= cfg.size()) {
            const std::size_t end       = cfg.find('\n', pos);
            const std::string_view line = (end == std::string::npos) ? std::string_view{cfg}.substr(pos)
                                                                     : std::string_view{cfg}.substr(pos, end - pos);
            if (line.size() > 2 && line.ends_with("=y")) {
                const std::string_view key = line.substr(0, line.size() - 2);
                // The old prefix starts with the new one, so test it first.
                if (key.starts_with(kOldPrefix)) {
                    const std::string_view x = key.substr(kOldPrefix.size());
                    if (x == "NONE") {
                        baseline = true;
                    } else if (!x.empty() && non_baseline.empty()) {
                        non_baseline = "x86-64-v" + normalize(x);
                    }
                } else if (key.starts_with(kNewPrefix)) {
                    const std::string_view x = key.substr(kNewPrefix.size());
                    if (x == "1") {
                        baseline = true;
                    } else if (!x.empty() && non_baseline.empty()) {
                        non_baseline = "x86-64-v" + normalize(x);
                    }
                }
            }
            if (end == std::string::npos) {
                break;
            }
            pos = end + 1;
        }
        if (!non_baseline.empty()) {
            return non_baseline;
        }
        return baseline ? "x86-64-v1" : std::string{};
    }

    // "<tool> <version>" from the /proc/version line: the first word-boundary
    // occurrence of "clang" or "gcc" (case-insensitive), then the version =
    // the next token, skipping a literal "version" keyword and a single glue
    // char ("clang-18" => "18"). No tool token, or no digit-led version => "".
    [[nodiscard]] std::string detect_compiler(std::string_view raw) {
        const std::string text      = lower(raw);
        const std::size_t clang_pos = text.find("clang");
        const std::size_t gcc_pos   = text.find("gcc");
        bool is_clang               = false;
        std::size_t pos             = 0;
        if (clang_pos != std::string::npos && (gcc_pos == std::string::npos || clang_pos < gcc_pos)) {
            is_clang = true;
            pos      = clang_pos;
        } else if (gcc_pos != std::string::npos) {
            is_clang = false;
            pos      = gcc_pos;
        } else {
            return {};
        }
        const std::string_view tool = is_clang ? "clang" : "gcc";
        const std::size_t tool_len  = tool.size();
        // Word boundary: not embedded in a longer token on either side.
        if (pos > 0 && std::isalnum(static_cast<unsigned char>(text.at(pos - 1)))) {
            return {};
        }
        if (pos + tool_len < text.size() && std::isalnum(static_cast<unsigned char>(text.at(pos + tool_len)))) {
            return {};
        }
        std::size_t i = pos + tool_len;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text.at(i)))) {
            ++i;
        }
        // Skip a literal "version" keyword ("clang version 22.1.8").
        if (i + 7 <= text.size() && text.compare(i, 7, "version") == 0) {
            const std::size_t j = i + 7;
            if (j >= text.size() || !std::isalnum(static_cast<unsigned char>(text.at(j)))) {
                i = j;
                while (i < text.size() && std::isspace(static_cast<unsigned char>(text.at(i)))) {
                    ++i;
                }
            }
        }
        // Skip a single glue char when the version is glued to the tool.
        if (i < text.size() && (text.at(i) == '-' || text.at(i) == '.') && i + 1 < text.size()
            && std::isdigit(static_cast<unsigned char>(text.at(i + 1)))) {
            ++i;
        }
        std::size_t ve = i;
        while (ve < text.size() && !std::isspace(static_cast<unsigned char>(text.at(ve)))) {
            ++ve;
        }
        if (i == ve) {
            return {};
        }
        std::string version = text.substr(i, ve - i);
        if (!version.empty() && version.back() == ',') {
            version.pop_back();  // "clang version 22.1.8, LLD ..."
        }
        if (version.empty() || !std::isdigit(static_cast<unsigned char>(version.front()))) {
            return {};
        }
        return std::string{tool} + " " + version;
    }

    // The date portion of a `uname -v` value: the string starts with the SMP/
    // PREEMPT build flags ("#1 SMP PREEMPT_DYNAMIC ...") and the date always
    // begins with a day-of-week abbreviation, so take the substring from the
    // first such token on; if the format is unexpected (none found), degrade
    // to the full trimmed value (never emptier than the input).
    [[nodiscard]] std::string build_date_from_uname_v(std::string_view uname_v) {
        const std::string_view full                = trim(uname_v);
        const std::array<std::string_view, 7> dows = {"Mon, ", "Tue, ", "Wed, ", "Thu, ", "Fri, ", "Sat, ", "Sun, "};
        std::size_t pos                            = std::string::npos;
        for (const auto& dow : dows) {
            if (const std::size_t found = full.find(dow); found != std::string::npos && (pos == std::string::npos || found < pos)) {
                pos = found;
            }
        }
        return std::string{trim(pos == std::string::npos ? full : full.substr(pos))};
    }

    // The bracketed token of a sysfs multi-option value ("always [madvise]
    // never" => "madvise"); "" when there are no (well-formed) brackets.
    [[nodiscard]] std::string bracketed_word(const std::string& value) {
        const std::size_t open = value.find('[');
        if (open == std::string::npos) {
            return {};
        }
        const std::size_t close = value.find(']', open + 1);
        if (close == std::string::npos || close <= open + 1) {
            return {};
        }
        return value.substr(open + 1, close - open - 1);
    }

    // Case-insensitive token equality (the mnemonic scan compares whole
    // whitespace/punctuation-delimited tokens, never a raw substring — so
    // "andnp" never matches "andn").
    [[nodiscard]] bool iequals(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a.at(i))) != std::tolower(static_cast<unsigned char>(b.at(i)))) {
                return false;
            }
        }
        return true;
    }

    // True iff the disassembly carries at least one of the curated BMI2/AVX2
    // mnemonics as a whole token (the D2b token-boundary contract).
    [[nodiscard]] bool has_isa_token(std::string_view disasm) {
        constexpr std::array<std::string_view, 5> kTokens{"andn", "shlx", "shrx", "rorx", "sarx"};
        std::size_t pos = 0;
        while (pos < disasm.size()) {
            while (pos < disasm.size() && !std::isalnum(static_cast<unsigned char>(disasm.at(pos)))) {
                ++pos;
            }
            if (pos >= disasm.size()) {
                break;
            }
            const std::size_t start = pos;
            while (pos < disasm.size() && std::isalnum(static_cast<unsigned char>(disasm.at(pos)))) {
                ++pos;
            }
            const std::string_view token = disasm.substr(start, pos - start);
            for (const std::string_view needle : kTokens) {
                if (iequals(token, needle)) {
                    return true;
                }
            }
        }
        return false;
    }

    // ---------------------------------------------------------------------
    // The real bindings (std-only; the driver_gate "real overload" shape).
    // Every read degrades to "" (or {}) on absence/EACCES/failure — the
    // no-signal contract; none of them prints or throws.
    // ---------------------------------------------------------------------

    // The booted release: /proc/sys/kernel/osrelease (a plain file — simpler
    // and shell-free than `uname -r`).
    [[nodiscard]] std::string uname_r_real() {
        return std::string{trim(read_small_file("/proc/sys/kernel/osrelease"))};
    }

    // The build-date portion: the /proc/version line from its first '#' on
    // ("#1 SMP PREEMPT_DYNAMIC Thu, ..." — the `uname -v` value).
    [[nodiscard]] std::string uname_v_real() {
        const std::string version = read_small_file("/proc/version");
        const std::size_t hash    = version.find('#');
        if (hash == std::string::npos) {
            return {};
        }
        return std::string{trim(version.substr(hash))};
    }

    // The whole /proc/version line (the compiler source of truth).
    [[nodiscard]] std::string proc_version_real() {
        return std::string{trim(read_small_file("/proc/version"))};
    }

    // The booted kernel's config: `zcat /proc/config.gz` when IKCONFIG is on;
    // otherwise the plain /boot/config-<release> file (release from the real
    // osrelease read). No in-tree gzip reader (D5); a machine with neither
    // degrades every kconfig-derived field to the empty-config rule.
    [[nodiscard]] std::string kconfig_real() {
        std::string cfg = run_command("zcat /proc/config.gz 2>/dev/null");
        if (cfg == "-1" || cfg.empty()) {
            const std::string release = uname_r_real();
            if (release.empty() || !is_name_safe(release)) {
                return {};
            }
            cfg = read_small_file("/boot/config-" + release);
        }
        return cfg;
    }

    // One /sys (or /proc/sys) attribute file by path; "" on EACCES/absent
    // (the sched/preempt file is root-only for a non-root app — D5 risk 4).
    [[nodiscard]] std::string sys_read_real(std::string_view path) {
        if (path.empty()) {
            return {};
        }
        return read_small_file(path);
    }

    // One sysctl value by dotted key: `sysctl -n <key>` (raw, the pure
    // extraction trims); the key is restricted to the dotted-key character
    // class before it reaches the shell.
    [[nodiscard]] std::string sysctl_get_real(std::string_view key) {
        if (!is_name_safe(key)) {
            return {};
        }
        std::string out = run_command("sysctl -n " + std::string{key} + " 2>/dev/null");
        if (out == "-1") {
            return {};
        }
        return out;
    }

    // The loaded module names in lsmod order (the first column of each line,
    // the header skipped); {} when lsmod is absent.
    [[nodiscard]] std::vector<std::string> loaded_modules_real() {
        std::vector<std::string> names{};
        const std::string out = run_command("lsmod 2>/dev/null");
        if (out == "-1") {
            return names;
        }
        bool header     = true;
        std::size_t pos = 0;
        while (pos <= out.size()) {
            const std::size_t end       = out.find('\n', pos);
            const std::string_view line = (end == std::string::npos) ? std::string_view{out}.substr(pos)
                                                                     : std::string_view{out}.substr(pos, end - pos);
            const std::string_view t    = trim(line);
            if (!header && !t.empty()) {
                std::size_t ws = 0;
                while (ws < t.size() && !std::isspace(static_cast<unsigned char>(t.at(ws)))) {
                    ++ws;
                }
                const std::string_view name = t.substr(0, ws);
                if (!name.empty()) {
                    names.emplace_back(name);
                }
            }
            header = false;
            if (end == std::string::npos) {
                break;
            }
            pos = end + 1;
        }
        return names;
    }

    // A module's on-disk filename: `modinfo -F filename <name>`; the name is
    // character-class guarded before it reaches the shell (a /proc/modules
    // name is filename-safe); "" when the module is unknown.
    [[nodiscard]] std::string module_filename_real(std::string_view name) {
        if (!is_name_safe(name)) {
            return {};
        }
        const std::string out = run_command("modinfo -F filename " + std::string{name} + " 2>/dev/null");
        if (out == "-1") {
            return {};
        }
        return std::string{trim(out)};
    }

    // A module file's disassembly, bounded by design (D5 risk 2): an
    // llvm-objdump availability probe (absent => "" forever), a per-process
    // 8-sample budget, a per-file cache (a repeat never re-dispatches), the
    // zstd path for .zst modules (zstdcat | llvm-objdump -d -), the raw path
    // otherwise, xz => "" (no unxz in the contract), and the 2 MiB cap.
    [[nodiscard]] std::string disassemble_real(std::string_view file) {
        if (file.empty() || !is_path_safe(file)) {
            return {};
        }
        const std::string key{file};
        std::string command{};
        if (key.ends_with(".zst")) {
            command = "zstdcat '" + key + "' 2>/dev/null | llvm-objdump -d - 2>/dev/null";
        } else if (!key.ends_with(".xz")) {
            command = "llvm-objdump -d '" + key + "' 2>/dev/null";
        }
        // (.xz leaves command "" = no signal, cached below as "")

        static std::mutex mutex{};
        static const bool available = [] {
            const std::string out = run_command("command -v llvm-objdump 2>/dev/null");
            return out != "-1" && !trim(out).empty();
        }();
        static std::size_t dispatched = 0;
        static std::unordered_map<std::string, std::string> cache{};

        std::string result{};
        bool run = false;
        {
            const std::scoped_lock lock{mutex};
            if (!available) {
                return {};
            }
            const auto it = cache.find(key);
            if (it != cache.end()) {
                return it->second;
            }
            if (command.empty() || dispatched >= kSampleBudget) {
                cache[key] = {};
                return {};
            }
            ++dispatched;
            run = true;
        }
        if (run) {
            result = run_command(command, kDisasmCap);
            if (result == "-1") {
                result = {};
            }
        }
        {
            const std::scoped_lock lock{mutex};
            cache[key] = result;
        }
        return result;
    }

}  // namespace

KernelInfo extract_kernel_info(const KernelInfoProbe& probe) {
    // Pure — no filesystem, no shell, no throws. Unbound predicates degrade
    // their field to "" (no signal); an empty kconfig degrades every
    // kconfig-derived field to "" too (no false "None"/"Disabled"/"Custom"
    // claims when there is no config at all).
    KernelInfo info{};

    // Kernel & Toolchain.
    info.release    = std::string{trim(ask_str(probe.uname_r))};
    info.build_date = build_date_from_uname_v(ask_str(probe.uname_v));  // date portion (SMP/PREEMPT flags stripped)
    info.compiler   = detect_compiler(ask_str(probe.proc_version));

    // CPU Arch Target (kconfig-derived; gated on a non-empty config).
    const std::string cfg = ask_str(probe.kconfig);
    if (!cfg.empty()) {
        if (cfg_has(cfg, "CONFIG_X86_NATIVE_CPU")) {
            info.target_arch = "Native";
        } else if (const std::string family = first_cpu_family(cfg); !family.empty()) {
            info.target_arch = "Family Optimized [" + family + "]";
        } else if (cfg_has(cfg, "CONFIG_GENERIC_CPU")) {
            info.target_arch = "Generic";
        } else {
            info.target_arch = "Custom";
        }
        info.isa_level = detect_isa_level(cfg);
    }

    // Optimization & LTO (kconfig-derived).
    if (!cfg.empty()) {
        if (cfg_has(cfg, "CONFIG_LTO_CLANG_FULL")) {
            info.lto_status = "Full LTO";
        } else if (cfg_has(cfg, "CONFIG_LTO_CLANG_THIN")) {
            info.lto_status = "Thin LTO";
        } else {
            info.lto_status = "None";
        }
        if (cfg_has(cfg, "CONFIG_CC_OPTIMIZE_FOR_PERFORMANCE_O3")) {
            info.optimization_flag = "-O3";
        } else if (cfg_has(cfg, "CONFIG_CC_OPTIMIZE_FOR_PERFORMANCE")) {
            info.optimization_flag = "-O2";
        }
        if (const std::string hz = cfg_val(cfg, "CONFIG_HZ"); !hz.empty()) {
            info.tick_rate = hz + " Hz";
        }
        info.sched_ext = cfg_has(cfg, "CONFIG_SCHED_CLASS_EXT") ? "Enabled" : "Disabled";
    }

    // Scheduling & Latency: the preemption model is two-tier — the live sysfs
    // word first (root-only, EACCES for a non-root app), the kconfig fallback
    // second (the dev box reads "Dynamic" via CONFIG_PREEMPT_DYNAMIC=y).
    if (const std::string preempt = std::string{trim(ask_str_arg(probe.sys_read, "/sys/kernel/debug/sched/preempt"))}; !preempt.empty()) {
        info.preemption_model = preempt;
    } else if (!cfg.empty()) {
        if (cfg_has(cfg, "CONFIG_PREEMPT_DYNAMIC")) {
            info.preemption_model = "Dynamic";
        } else if (cfg_has(cfg, "CONFIG_PREEMPT")) {
            info.preemption_model = "Full";
        } else if (cfg_has(cfg, "CONFIG_PREEMPT_VOLUNTARY")) {
            info.preemption_model = "Voluntary";
        }
    }

    // Runtime Subsystems (sys + sysctl).
    if (const std::string mglru = std::string{trim(ask_str_arg(probe.sys_read, "/sys/kernel/mm/lru_gen/enabled"))}; !mglru.empty()) {
        info.mglru = (mglru == "0" || mglru == "0x0000") ? "Disabled" : "Active (" + mglru + ")";
    }
    info.thp            = bracketed_word(ask_str_arg(probe.sys_read, "/sys/kernel/mm/transparent_hugepage/enabled"));
    info.clocksource    = std::string{trim(ask_str_arg(probe.sys_read, "/sys/devices/system/clocksource/clocksource0/current_clocksource"))};
    info.tcp_congestion = std::string{trim(ask_str_arg(probe.sysctl_get, "net.ipv4.tcp_congestion_control"))};

    // Instruction-set validation: the first loaded module that resolves to a
    // file and disassembles; "BMI2/AVX2 verified" when its disassembly carries
    // a curated BMI2/AVX2 mnemonic, "" (not checked) otherwise.
    for (const std::string& name : ask_vec(probe.loaded_modules)) {
        const std::string file   = ask_str_arg(probe.module_filename, name);
        const std::string disasm = file.empty() ? std::string{} : ask_str_arg(probe.disassemble, file);
        if (disasm.empty()) {
            continue;  // unresolvable / xz / no objdump: keep looking
        }
        info.instruction_validation = has_isa_token(disasm) ? "BMI2/AVX2 verified" : "";
        break;  // the first disassembled module is the sample
    }

    return info;
}

KernelInfo extract_kernel_info() {
    // The real probe wiring (the driver_gate real-overload precedent): every
    // system fact behind the injectable KernelInfoProbe, then the pure
    // extraction once.
    KernelInfoProbe probe{};
    probe.uname_r         = uname_r_real;
    probe.uname_v         = uname_v_real;
    probe.proc_version    = proc_version_real;
    probe.kconfig         = kconfig_real;
    probe.sys_read        = sys_read_real;
    probe.sysctl_get      = sysctl_get_real;
    probe.loaded_modules  = loaded_modules_real;
    probe.module_filename = module_filename_real;
    probe.disassemble     = disassemble_real;
    return extract_kernel_info(probe);
}

}  // namespace kernel_info
