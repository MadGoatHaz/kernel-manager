#!/usr/bin/env bash
# run_all_tests.sh — unified test runner for the kernel-manager test suite.
#
# Runs all 20 test harnesses in sequence, reports pass/fail for each, and
# returns a non-zero exit code if any test fails. Works from the source
# tree or from the build directory (it cd's to the source root itself).
#
# Usage:
#   tests/run_all_tests.sh           # run all tests
#   tests/run_all_tests.sh k5 k18    # run only the named harnesses
#
# The harness names are the short identifiers used in the run_*.sh files:
#   chunk1, chunk2, chunk2_ui, k5, k6, k7, k8, k11, k12, k13, k14, k15,
#   k16, k17, k18, k19, k20, k21, k22, k23
#
# Exit codes:
#   0  all requested tests passed
#   1  at least one test failed
#   2  usage error (unknown harness name)

set -uo pipefail

# Resolve the source root (this script lives in tests/).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

# Map harness short names to their run scripts.
declare -A HARNESS_SCRIPTS=(
    [chunk1]="tests/run_chunk1.sh"
    [chunk2]="tests/run_chunk2.sh"
    [chunk2_ui]="tests/run_chunk2_ui.sh"
    [k5]="tests/run_k5.sh"
    [k6]="tests/run_k6.sh"
    [k7]="tests/run_k7.sh"
    [k8]="tests/run_k8.sh"
    [k11]="tests/run_k11.sh"
    [k12]="tests/run_k12.sh"
    [k13]="tests/run_k13.sh"
    [k14]="tests/run_k14.sh"
    [k15]="tests/run_k15.sh"
    [k16]="tests/run_k16.sh"
    [k17]="tests/run_k17.sh"
    [k18]="tests/run_k18.sh"
    [k19]="tests/run_k19.sh"
    [k20]="tests/run_k20.sh"
    [k21]="tests/run_k21.sh"
    [k22]="tests/run_k22.sh"
    [k23]="tests/run_k23.sh"
)

# Default: run all harnesses in a sensible order (pure-C++ first, then
# build-dependent, then bash-only). This order minimizes the chance that
# a build-dependent test fails early and masks a pure-C++ failure.
ALL_HARNESSES=(
    chunk2 k5 k6 k18 k19 k20 k22 k23
    k7 k8 k11 k13 k14 k15
    chunk1 chunk2_ui k12
    k16 k17
    k21
)

# Parse arguments: either a list of harness names, or nothing (all).
REQUESTED=()
if [[ $# -gt 0 ]]; then
    for name in "$@"; do
        if [[ -z "${HARNESS_SCRIPTS[$name]:-}" ]]; then
            echo "error: unknown harness '$name'" >&2
            echo "valid names: ${!HARNESS_SCRIPTS[*]}" >&2
            exit 2
        fi
        REQUESTED+=("$name")
    done
else
    REQUESTED=("${ALL_HARNESSES[@]}")
fi

PASS=0
FAIL=0
FAILED_NAMES=()
TOTAL=${#REQUESTED[@]}
IDX=0

echo "================================================================"
echo " kernel-manager test suite: $TOTAL harness(es) requested"
echo " source root: $ROOT"
echo "================================================================"
echo

for name in "${REQUESTED[@]}"; do
    IDX=$((IDX + 1))
    script="${HARNESS_SCRIPTS[$name]}"
    echo "----------------------------------------------------------------"
    echo " [$IDX/$TOTAL] $name  ($script)"
    echo "----------------------------------------------------------------"

    if [[ ! -x "$script" ]]; then
        chmod +x "$script" || true
    fi

    # Run the harness, capturing its output. Each harness is self-contained
    # and prints its own pass/fail diagnostics; we just need its exit code.
    if bash "$script" 2>&1; then
        PASS=$((PASS + 1))
        echo " [PASS] $name"
    else
        FAIL=$((FAIL + 1))
        FAILED_NAMES+=("$name")
        echo " [FAIL] $name"
    fi
    echo
done

echo "================================================================"
echo " SUMMARY: $PASS passed, $FAIL failed, $TOTAL total"
if [[ $FAIL -gt 0 ]]; then
    echo " FAILED: ${FAILED_NAMES[*]}"
fi
echo "================================================================"

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
exit 0
