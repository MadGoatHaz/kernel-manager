#!/usr/bin/env bash
# common.sh — shared build-artifact resolution for the standalone test
# harnesses. Sourced by every run_*.sh that needs fmt / frozen / cxxbridge
# artifacts from the project's CMake build dir.
#
# Background: the harnesses compile a subset of the real project sources
# with a standalone g++ command and link against the static libraries that
# the CMake build produces (fmt, the Rust cxxbridge crate, etc.). Two
# environment differences break the old hard-coded paths:
#
#   1. Build-type dependent library name. fmt's CMake adds the "d" debug
#      postfix (FMT_DEBUG_POSTFIX=d, on by default) in Debug builds: the
#      library is build/_deps/fmt-build/libfmtd.a, not libfmt.a. CI builds
#      with CMAKE_BUILD_TYPE=Debug; local dev often uses RelWithDebInfo
#      (no postfix). The harnesses must accept either.
#
#   2. CPM source cache. When CPM_SOURCE_CACHE is set (CI does this, so the
#      fmt/frozen/corrosion downloads survive across runs and are shared
#      with the checks.yml job), CPM keeps the extracted sources under the
#      cache dir (e.g. $HOME/.cache/cpm/fmt/<hash>) and does not always
#      mirror them into build/_deps/<name>-src. The include dir for
#      frozen in particular can live only in the cache. The harnesses must
#      be able to find headers from the cache as well as from build/_deps.
#
#   3. Multi-worktree dev. When a harness runs from a secondary git
#      worktree that has not been built yet, the artifacts are only in the
#      main worktree's build/. The old bootstrap logic used
#      "git worktree list" for this; in a CI container that is a shallow
#      checkout inside Docker, the git discovery can fail
#      ("not a git repository ... filesystem boundary"). We keep the
#      worktree bootstrap but guard it so a git failure is never fatal when
#      the artifacts are already present locally.
#
# Usage (inside a run_*.sh that has already done `cd "$(dirname "$0")/.."`):
#
#   ROOT="$(pwd)"
#   BUILD="$ROOT/build"
#   source "$ROOT/tests/common.sh"
#
#   # Resolve only what this harness needs. Each function prints the
#   # resolved path to stdout and returns non-zero if the artifact cannot
#   # be found anywhere.
#   FMT_INC="$(resolve_fmt_include)"
#   FMT_LIB="$(resolve_fmt_lib)"
#   ...
#
# The functions are idempotent and cheap (path tests only).

# --- helpers ----------------------------------------------------------------

# Print the first existing path from the arguments, else exit 1.
_first_existing() {
    local p
    for p in "$@"; do
        if [[ -e "$p" ]]; then
            printf '%s\n' "$p"
            return 0
        fi
    done
    return 1
}

# Is the current ROOT a usable git repository? Returns 0 if `git rev-parse`
# succeeds inside $ROOT, 1 otherwise. Used to decide whether the worktree
# bootstrap is even worth trying (in CI Docker this can fail for harmless
# reasons; the artifacts may already be local).
_git_usable() {
    local root="${1:-$ROOT}"
    command -v git >/dev/null 2>&1 || return 1
    git -C "$root" rev-parse --git-dir >/dev/null 2>&1
}

# Resolve the main worktree root via `git worktree list`, if possible.
# Prints the main worktree path to stdout; returns 1 if git is unusable or
# the current worktree *is* the main one (in which case there is nothing to
# bootstrap from elsewhere).
_main_worktree_root() {
    local root="${1:-$ROOT}"
    _git_usable "$root" || return 1
    local main_root
    main_root="$(git -C "$root" worktree list --porcelain 2>/dev/null \
        | awk '/^worktree /{print $2; exit}')" || return 1
    # If the main worktree is the current one, there is no other source.
    [[ -n "$main_root" && "$main_root" != "$root" ]] || return 1
    printf '%s\n' "$main_worktree"
}

# Try to copy a missing build artifact from the main worktree's build dir
# into this worktree's build dir. No-op (success) if the artifact already
# exists here; failure only if it is missing everywhere and git is the only
# possible source.
_bootstrap_artifact() {
    local target="$1"        # absolute path in this worktree's build/
    local rel="${target#"${BUILD}"/}"
    [[ "$target" == "$BUILD"/* ]] || { echo "common.sh: bootstrap target must be under $BUILD: $target" >&2; return 1; }
    [[ -e "$target" ]] && return 0   # already present — nothing to do

    if ! _git_usable "$ROOT"; then
        # Not a git repo (e.g. a tarball or a CI checkout where git
        # discovery is blocked): the artifact must already be local or it
        # is genuinely missing. Report and let the caller decide.
        echo "common.sh: artifact not found and git unavailable to bootstrap: $rel" >&2
        return 1
    fi

    local main_root
    if main_root="$(_main_worktree_root "$ROOT")"; then
        local src="$main_root/build/$rel"
        if [[ -e "$src" ]]; then
            mkdir -p "$(dirname "$target")"
            cp -r "$src" "$target"
            echo "common.sh: bootstrapped $rel from main worktree $main_root" >&2
            return 0
        fi
        echo "common.sh: artifact not in main worktree either: $src" >&2
        return 1
    fi

    # git works but there is no distinct main worktree (we are the main
    # one): the artifact is simply missing.
    echo "common.sh: artifact not found (no other worktree to copy from): $rel" >&2
    return 1
}

# --- artifact resolvers ------------------------------------------------------
# Each resolver:
#   1. Checks the local build/_deps or build/ location(s).
#   2. Falls back to the CPM source cache for *headers* (the cache is the
#      canonical source location when CPM_SOURCE_CACHE is set).
#   3. Falls back to bootstrapping from the main worktree.
# Prints the resolved path on success; returns non-zero otherwise.

resolve_fmt_include() {
    local candidates=(
        "$BUILD/_deps/fmt-src/include"
    )
    # CPM cache: $CPM_SOURCE_CACHE (or $HOME/.cache/cpm by default) /fmt/<tag-or-hash>/include
    # Note: CPM_SOURCE_CACHE is only set in the "Configure CMake" step of CI;
    # the test step doesn't inherit it, so fall back to the conventional path.
    local cpm_cache="${CPM_SOURCE_CACHE:-$HOME/.cache/cpm}"
    if [[ -d "$cpm_cache" ]]; then
        local d
        for d in "$cpm_cache"/fmt/*/; do
            [[ -d "$d/include" ]] && candidates+=("$d/include")
        done
    fi
    local inc
    if inc="$(_first_existing "${candidates[@]}")"; then
        printf '%s\n' "$inc"
        return 0
    fi
    # Last resort: bootstrap the fmt-src tree from the main worktree.
    if _bootstrap_artifact "$BUILD/_deps/fmt-src"; then
        printf '%s\n' "$BUILD/_deps/fmt-src/include"
        return 0
    fi
    echo "common.sh: fmt include dir not found (tried: ${candidates[*]})" >&2
    return 1
}

resolve_fmt_lib() {
    # Accept both the Debug (libfmtd.a) and non-Debug (libfmt.a) names.
    local candidates=(
        "$BUILD/_deps/fmt-build/libfmtd.a"
        "$BUILD/_deps/fmt-build/libfmt.a"
    )
    local lib
    if lib="$(_first_existing "${candidates[@]}")"; then
        printf '%s\n' "$lib"
        return 0
    fi
    # Bootstrap the fmt-build tree from the main worktree, then retry.
    if _bootstrap_artifact "$BUILD/_deps/fmt-build"; then
        if lib="$(_first_existing "${candidates[@]}")"; then
            printf '%s\n' "$lib"
            return 0
        fi
    fi
    echo "common.sh: fmt static lib not found (tried: ${candidates[*]})" >&2
    return 1
}

resolve_frozen_include() {
    local candidates=(
        "$BUILD/_deps/frozen-src/include"
    )
    # CPM cache: $CPM_SOURCE_CACHE (or $HOME/.cache/cpm by default) /frozen/<tag-or-hash>/include
    local cpm_cache="${CPM_SOURCE_CACHE:-$HOME/.cache/cpm}"
    if [[ -d "$cpm_cache" ]]; then
        local d
        for d in "$cpm_cache"/frozen/*/; do
            [[ -d "$d/include" ]] && candidates+=("$d/include")
        done
    fi
    local inc
    if inc="$(_first_existing "${candidates[@]}")"; then
        printf '%s\n' "$inc"
        return 0
    fi
    if _bootstrap_artifact "$BUILD/_deps/frozen-src"; then
        printf '%s\n' "$BUILD/_deps/frozen-src/include"
        return 0
    fi
    echo "common.sh: frozen include dir not found (tried: ${candidates[*]})" >&2
    return 1
}

resolve_cxxbridge_include() {
    local inc="$BUILD/corrosion_generated/cxxbridge/config-option-lib-cxxbridge/include"
    [[ -d "$inc" ]] && { printf '%s\n' "$inc"; return 0; }
    if _bootstrap_artifact "$BUILD/corrosion_generated"; then
        [[ -d "$inc" ]] && { printf '%s\n' "$inc"; return 0; }
    fi
    echo "common.sh: cxxbridge include dir not found: $inc" >&2
    return 1
}

resolve_cxxbridge_libs() {
    # Prints two lines: the C++ wrapper lib, then the Rust static lib.
    local a="$BUILD/libconfig-option-lib-cxxbridge.a"
    local b="$BUILD/libconfig_option_lib.a"
    [[ -f "$a" ]] || _bootstrap_artifact "$a" || true
    [[ -f "$b" ]] || _bootstrap_artifact "$b" || true
    if [[ -f "$a" && -f "$b" ]]; then
        printf '%s\n%s\n' "$a" "$b"
        return 0
    fi
    echo "common.sh: cxxbridge static libs not found ($a / $b)" >&2
    return 1
}

resolve_compile_options_h() {
    local h="$BUILD/compile_options.hpp"
    [[ -f "$h" ]] && { printf '%s\n' "$h"; return 0; }
    if _bootstrap_artifact "$h"; then
        printf '%s\n' "$h"
        return 0
    fi
    echo "common.sh: compile_options.hpp not found: $h" >&2
    return 1
}

resolve_autogen_dir() {
    local d="$BUILD/kernel-manager_autogen"
    [[ -d "$d" ]] && { printf '%s\n' "$d"; return 0; }
    if _bootstrap_artifact "$d"; then
        [[ -d "$d" ]] && { printf '%s\n' "$d"; return 0; }
    fi
    echo "common.sh: kernel-manager_autogen dir not found: $d" >&2
    return 1
}
