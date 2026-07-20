#!/usr/bin/env bash
# tools/run_static_analysis.sh — static analysis gate for the vaios kernel.
#
# Complements the ASan/UBSan runtime suite (tools/run_tests.sh): those only see
# paths the tests execute, this reasons about the shipping sources statically.
# Two passes, both must be clean:
#   1. cppcheck   — warning+error severities over kernel/ + portable/, minus a
#                   justified false-positive baseline (tools/cppcheck-suppressions.txt).
#   2. -fanalyzer — a GCC static-analyzer build of the host test suite
#                   (VAIOS_TEST_ANALYZER=ON); any -Wanalyzer finding fails.
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
echo "=== static analysis: no MMIO outside portable/ ==="
# An integer literal cast to a pointer is memory-mapped I/O. In kernel/ or
# include/ that is ALWAYS a missing v_port_* facade function, never something to
# suppress: the address, the access width and the ordering rules are all
# properties of a specific core or SoC, so the kernel cannot own them. There is
# deliberately no escape hatch — a hit here is a design fix, not a baseline
# entry. portable/ is out of scope (raw access there is the whole point), and so
# are examples/ and tools/, where poking a known address IS the experiment.
#
# Scoped by directory rather than by CodeQL config on purpose: `paths-ignore`
# is silently ignored for a built C/C++ analysis (see the header comment in
# .github/workflows/codeql.yml), so grep is the tool that can actually express
# "everywhere except this directory".
#
# Deliberately NOT clang-tidy's performance-no-int-to-ptr: that check fires on
# casts from integer *variables* and exempts constant addresses, which is the
# exact inverse of what is wanted here — it misses every MMIO site in this tree
# while flagging four legitimate uintptr_t conversions in kernel/memory/memory.c.
MMIO_RE='\([[:space:]]*[A-Za-z_][A-Za-z0-9_[:space:]]*\*[[:space:]]*\)[[:space:]]*0[xX][0-9A-Fa-f]'
mmio="$( cd "$ROOT_DIR" && grep -rnE --include='*.c' --include='*.h' \
         "$MMIO_RE" kernel/ include/ || true )"
if [ -z "$mmio" ]; then
  echo "  PASS: no memory-mapped access in kernel/ or include/"
else
  echo "  FAIL: $(printf '%s\n' "$mmio" | wc -l) memory-mapped access(es) outside portable/:"
  printf '%s\n' "$mmio" | sed 's/^/    /'
  echo "    -> move behind a v_port_* function in portable/<arch>/"
  rc=1
fi

echo ""
if [ "$rc" -eq 0 ]; then
  echo "=== static analysis: CLEAN ==="
else
  echo "=== static analysis: FINDINGS ==="
fi
exit "$rc"
