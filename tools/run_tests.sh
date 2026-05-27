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

# -----------------------------------------------------------------------------
# Cross-binary summary table.
#
# Parses every "=== Suite: NAME ===" header and counts the PASS/FAIL lines
# that TEST_RUN emits underneath it (each test trails its assertion count in
# parens, which the test framework prints as e.g. "PASS (3)" / "FAIL (1/4)").
# Strips ANSI colour first so awk only sees plain text.
# -----------------------------------------------------------------------------
echo ""
cat "$LOG_DIR/main.log" "$LOG_DIR/utils.log" \
  | sed -E 's/\x1B\[[0-9;]*[A-Za-z]//g' \
  | awk '
      BEGIN {
        # Force ns/maxlen into numeric context — awk treats uninitialized
        # variables as the empty string, so s_name[ns] would index the
        # array with "" instead of 0 on the very first store, silently
        # losing the first suite (Memory Allocator) and producing a stray
        # empty row at the top of the table.
        ns = 0; maxlen = 0
        tot_pass = 0; tot_fail = 0; tot_tests = 0
        suite = ""; pass = 0; fail = 0
      }
      function flush() {
        if (suite != "") {
          s_name[ns]  = suite
          s_pass[ns]  = pass
          s_fail[ns]  = fail
          s_total[ns] = pass + fail
          ns++
          tot_pass  += pass
          tot_fail  += fail
          tot_tests += pass + fail
          if (length(suite) > maxlen) maxlen = length(suite)
        }
      }
      /^=== Suite:/ {
        flush()
        suite = $0
        sub(/^=== Suite: /, "", suite)
        sub(/ ===$/, "", suite)
        pass = 0; fail = 0
      }
      /^[[:space:]]+.*PASS [(]/ { pass++ }
      /^[[:space:]]+.*FAIL [(]/ { fail++ }
      END {
        flush()
        GREEN = "\033[32m"; RED = "\033[31m"; BOLD = "\033[1m"; RST = "\033[0m"
        if (maxlen < 20) maxlen = 20
        fmt = sprintf("  %%-%ds  %%5s   %%5s   %%5s\n", maxlen)
        printf BOLD "========== TEST SUMMARY ==========" RST "\n"
        printf BOLD fmt RST, "Suite", "Tests", "Pass", "Fail"
        sep = ""
        for (i = 0; i < maxlen + 24; i++) sep = sep "-"
        printf "  %s\n", sep
        for (i = 0; i < ns; i++) {
          fcol = (s_fail[i] > 0) ? RED : GREEN
          printf "  %-*s  %5d   " fcol "%5d" RST "   " fcol "%5d" RST "\n", \
                 maxlen, s_name[i], s_total[i], s_pass[i], s_fail[i]
        }
        printf "  %s\n", sep
        gcol = (tot_fail > 0) ? RED : GREEN
        printf "  " BOLD "%-*s" RST "  " BOLD "%5d" RST "   " \
               gcol BOLD "%5d" RST "   " gcol BOLD "%5d" RST "\n", \
               maxlen, "GRAND TOTAL", tot_tests, tot_pass, tot_fail
        printf BOLD "==================================" RST "\n"
      }
  '

EXIT_CODE=$(( MAIN_EXIT | UTILS_EXIT ))
echo ""
if [ $EXIT_CODE -eq 0 ]; then
    echo -e "\033[1;32m=== ALL TESTS PASSED ===\033[0m"
else
    echo -e "\033[1;31m=== SOME TESTS FAILED ===\033[0m"
fi
exit $EXIT_CODE
