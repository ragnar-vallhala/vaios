#!/usr/bin/env bash
# tools/run_static_analysis.sh — static analysis gate for the vaios kernel.
#
# Complements the ASan/UBSan runtime suite (tools/run_tests.sh): those only see
# paths the tests execute, this reasons about the shipping sources statically.
# Three passes, all must be clean:
#   1. cppcheck   — warning+error severities over kernel/ + portable/, minus a
#                   justified false-positive baseline (tools/cppcheck-suppressions.txt).
#   2. -fanalyzer — a GCC static-analyzer build of the host test suite
#                   (VAIOS_TEST_ANALYZER=ON); any -Wanalyzer finding fails.
#   3. portability — tools/check_portability.sh: no inline asm, arch macros,
#                   vendor includes, or MMIO in kernel/ or include/.
#
# Missing tools are treated as SKIP (exit 0) so the same script is safe on a
# machine without cppcheck/gcc, mirroring the renode_*.sh convention.
#
# Exit: 0 = clean (or skipped), 1 = findings.
set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SUPPRESS="$ROOT_DIR/tools/cppcheck-suppressions.txt"
BUILD_DIR="$ROOT_DIR/build_analyzer"
JOBS="$(nproc 2>/dev/null || echo 2)"
rc=0

echo "=== static analysis: cppcheck ==="
if command -v cppcheck >/dev/null 2>&1; then
  # Run from the repo root with RELATIVE scan paths: cppcheck matches the
  # suppression-file paths (e.g. kernel/structure.c) literally against the paths
  # it analyzes, so relative in / relative out keeps the baseline readable.
  # enable=warning implies error; style/perf/portability are left out of the
  # gate (higher false-positive rate). --error-exitcode=2 fails on any reported,
  # non-suppressed issue. --inline-suppr honours in-source // cppcheck-suppress.
  if ( cd "$ROOT_DIR" && cppcheck --enable=warning --inline-suppr \
       --error-exitcode=2 -j"$JOBS" --quiet \
       --suppressions-list="$SUPPRESS" \
       --suppress=missingInclude --suppress=missingIncludeSystem \
       -I include -I portable/cortex-m4 \
       kernel/ portable/cortex-m4/ ); then
    echo "  PASS: cppcheck clean"
  else
    echo "  FAIL: cppcheck reported findings above"; rc=1
  fi
else
  echo "  SKIP: cppcheck not installed"
fi

echo ""
echo "=== static analysis: gcc -fanalyzer ==="
if command -v gcc >/dev/null 2>&1 && command -v cmake >/dev/null 2>&1; then
  log="$(mktemp)"
  if cmake -S "$ROOT_DIR/tests" -B "$BUILD_DIR" \
        -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DVAIOS_TEST_SANITIZE=OFF -DVAIOS_TEST_ANALYZER=ON \
        --fresh >/dev/null 2>&1 \
     && cmake --build "$BUILD_DIR" --parallel >"$log" 2>&1; then
    # The build itself succeeds (no -Werror); a finding is any -Wanalyzer line.
    hits="$(grep -cE '\[-Wanalyzer' "$log" || true)"
    if [ "$hits" -eq 0 ]; then
      echo "  PASS: no -Wanalyzer findings"
    else
      echo "  FAIL: $hits -Wanalyzer finding(s):"
      grep -E 'warning:.*\[-Wanalyzer' "$log" | sed 's/^/    /' | sort -u
      rc=1
    fi
  else
    echo "  FAIL: analyzer build failed"; tail -20 "$log" | sed 's/^/    /'; rc=1
  fi
  rm -f "$log"
else
  echo "  SKIP: gcc/cmake not installed"
fi

echo ""
# Portability tripwire: no inline asm, arch macros, vendor includes, or memory-
# mapped I/O in kernel/ or include/. Owns the full grep-based check (it prints
# its own header + PASS/FAIL); run standalone with tools/check_portability.sh.
bash "$ROOT_DIR/tools/check_portability.sh" || rc=1

echo ""
if [ "$rc" -eq 0 ]; then
  echo "=== static analysis: CLEAN ==="
else
  echo "=== static analysis: FINDINGS ==="
fi
exit "$rc"
