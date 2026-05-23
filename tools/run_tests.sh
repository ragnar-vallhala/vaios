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

"$BUILD_DIR/vaios_tests"
MAIN_EXIT=$?

echo ""
echo "=== Running utils-only tests (separate binary) ==="
"$BUILD_DIR/vaios_utils_tests"
UTILS_EXIT=$?

EXIT_CODE=$(( MAIN_EXIT | UTILS_EXIT ))
echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo -e "\033[1;32m=== ALL TESTS PASSED ===\033[0m"
else
    echo -e "\033[1;31m=== SOME TESTS FAILED ===\033[0m"
fi
exit $EXIT_CODE
