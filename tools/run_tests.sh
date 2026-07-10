#!/bin/bash
# run_tests.sh – Build and run the vaios RTOS host-native test suite
# Usage: ./tools/run_tests.sh [--verbose]

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$ROOT_DIR/build_tests"
VERBOSE=${1:-}

echo "=== vaios unit test build ==="
echo "Root:  $ROOT_DIR"
echo "Build: $BUILD_DIR"

# Configure (standalone, not as subdirectory of the cross-compile build)
cmake -S "$ROOT_DIR/tests" \
      -B "$BUILD_DIR" \
      -DCMAKE_C_COMPILER=gcc \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_SYSTEM_NAME=Linux \
      -DCMAKE_C_FLAGS="-DCORTEX_M4" \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      --fresh

# Build
cmake --build "$BUILD_DIR" --parallel


echo ""
echo "=== Running tests ==="

# Tee each binary's output so the user sees it live, and we have logs to parse
# for the cross-binary summary table at the end.
LOG_DIR="$(mktemp -d)"
trap 'rm -rf "$LOG_DIR"' EXIT

"$BUILD_DIR/vaios_tests" | tee "$LOG_DIR/main.log"
MAIN_EXIT=${PIPESTATUS[0]}

echo ""
echo "=== Running utils-only tests (separate binary) ==="
"$BUILD_DIR/vaios_utils_tests" | tee "$LOG_DIR/utils.log"
UTILS_EXIT=${PIPESTATUS[0]}

echo ""
echo "=== Running devfs tests (separate binary: DEVFS=1) ==="
"$BUILD_DIR/vaios_devfs_tests" | tee "$LOG_DIR/devfs.log"
DEVFS_EXIT=${PIPESTATUS[0]}

# -----------------------------------------------------------------------------
# Cross-binary summary table. Renderer is shared with tools/run_hw_tests.sh —
# tools/lib/test_summary.awk owns the format. ANSI is stripped before awk
# so the parser only deals with plain text.
# -----------------------------------------------------------------------------
echo ""
cat "$LOG_DIR/main.log" "$LOG_DIR/utils.log" \
  | sed -E 's/\x1B\[[0-9;]*[A-Za-z]//g' \
  | awk -v title="HOST TEST SUMMARY" -f "$SCRIPT_DIR/lib/test_summary.awk"

EXIT_CODE=$(( MAIN_EXIT | UTILS_EXIT | DEVFS_EXIT ))
echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo -e "\033[1;32m=== ALL TESTS PASSED ===\033[0m"
else
    echo -e "\033[1;31m=== SOME TESTS FAILED ===\033[0m"
fi
exit $EXIT_CODE
