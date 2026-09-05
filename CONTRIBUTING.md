# Contributing to kernel-manager

Thanks for your interest! This project holds a high quality bar and every change is
expected to keep it. Read this before opening a PR — the standards below are
**mandatory**, and CI + the local QA gate enforce them.

## Code Quality Standards (MANDATORY)

### Formatting

- clang-format **16** (pinned in `.github/workflows/checks.yml` via
  `jidicula/clang-format-action` `clang-format-version: '16'`, `check-path: 'src'`,
  `exclude-regex: 'src/ini.hpp'`; the same pin is documented in the `.clang-format`
  header comment).
- Run: `clang-format --style=file --fix src/*.cpp src/*.hpp`
- **Never run a different major version on `src/`.** clang-format 22 diverges from 16
  on ~3 spots (member-pointer spacing in `conf-window.cpp`, one `.arg()` continuation
  indent in `km-window.cpp`) and its output **fails the CI check**. If you must bump
  the version, raise the `checks.yml` pin and the `.clang-format` note **together** and
  re-format the whole tree.
- The style is `BasedOnStyle: WebKit` (see `.clang-format` for the project overrides,
  incl. `SpacesBeforeTrailingComments: 2`).

### Linting

- clang-tidy with `WarningsAsErrors: '*'` (see `.clang-tidy`) — the run must be at
  **0 diagnostics**: `clang-tidy -p build/Debug src/*.cpp` (after a `./configure.sh
  -t=Debug --use_clang` + `./build.sh`).
- **No blanket `// NOLINT`** — only specific, named-check suppressions, each with a
  rationale comment explaining why the check is wrong for that line.
- Existing justified suppressions (keep them; they are permanent and specific):
  `src/utils.cpp` (popen — legitimate shell commands) and `src/bootloader.cpp`
  (`command -v` probe).
- **Do not remove the load-bearing `.clang-tidy` `ExcludeHeaderFilterRegex`** — it
  scopes reporting to project `src/` and is what keeps the not-owned
  fmt / frozen / cxxbridge / generated `ui_*.h` headers (and the vendored `src/ini.hpp`)
  out of the run. Removing it re-opens hundreds of dependency/generated-header
  diagnostics.

### Build

- C++23, GCC 16 / Clang (Qt6), **0 warnings required** on a full build.
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j"$(nproc)"`
  (the CI linter path uses `./configure.sh -t=Debug --use_clang && ./build.sh`).
- The only expected non-code note is the documented `lto-wrapper` serial-LTRANS tooling
  line; any real `warning:` in your own code is a defect to fix, not to suppress.

### Testing

- **15 standalone harnesses** (`tests/run_k*.sh` + `tests/run_chunk*.sh`) — **ALL must
  pass** with 0 real-FAIL lines:
  `for t in tests/run_*.sh; do bash "$t"; done`
- **Offscreen smoke:** `QT_QPA_PLATFORM=offscreen timeout 4 ./build/kernel-manager` —
  expect `rc=124` (killed by the timeout = the app stayed alive) and **no crash**
  (0-byte stdout/stderr is the clean baseline).
- The harness runners **compile `src` directly with the system GCC (C++23)** — there is
  no CTest. Any **API addition (new header member, function signature, compile
  definition) needs a companion in the matching `tests/run_kNN.sh`**, or that harness
  will fail to compile. (Example: the `APP_VERSION` definition added a `run_k12.sh`
  companion that derives it from the CMakeLists `VERSION` line.)

### Review Process

- **Reviewers modify ZERO source files** — a review is a read-audit + independent test
  re-run; a failing review is *reported* with exact diagnostics, never fixed in-place.
- Every merge is `--no-ff` with a descriptive message.
- Branch naming: `branch/<feature>` or `branch/chunk-<N>`.
- One atomic single-file micro-chunk per branch where possible (a new script + its CMake
  registration, or a test + its `run_kNN.sh`, is one unit); keep the diff tight.

### Release Process

1. Bump `CMakeLists.txt` `project(... VERSION X.Y.Z)` (the single source of truth the
   UI version display auto-tracks).
2. Re-pin `PKGBUILD`: `pkgver` → X.Y.Z, `_commit` → the **version-bump commit** (one
   before the tag, so the archived source is a stable non-circular snapshot), and
   **`sha256sums`** from a **real double-fetched** `git archive` (fetch twice, confirm
   byte-identical — never a placeholder).
3. Cut an **annotated tag** on the re-pin commit.
4. Push the tag + `main` to origin.
5. Create the **GitHub release** with the source tarball (the AUR build target) and the
   Changelog entry as the notes.
6. **Always** `gh --repo MadGoatHaz/kernel-manager` — from this working directory `gh`
   infers the **upstream** `CachyOS/kernel-manager` and 404s / runs the wrong repo.

## Project Structure

- `src/` — the C++23 / Qt6 application (main window, configure dialog, install engine,
  bootloader + distro detection, terminal helpers)
- `tests/` — 15 standalone harnesses (g++ C++23, no CTest)
- `config-option-lib/` — Rust crate (config-option parsing via Corrosion / cxx)
- `lang/` — Qt translation catalogs (16 `.ts`; resync manually with
  `lupdate6 src/*.cpp src/*.hpp src/*.ui -ts lang/kernel-manager_*.ts` — there is no
  CMake target)
- `Work/Docs/` — project documentation (gitignored, local-only)
- `plans/` — dev-cycle plans (gitignored, local-only)

## Feedback & Questions

Report bugs and suggest features via the [issue
tracker](https://github.com/MadGoatHaz/kernel-manager/issues), or open a pull request
against `main`. See [Changelog.md](Changelog.md) for the release history. (The
per-cycle dev state and handover notes live in the local-only `Work/Docs/` and
`plans/` directories, which are gitignored and not published.)
