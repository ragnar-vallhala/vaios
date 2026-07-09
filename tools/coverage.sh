#!/usr/bin/env bash
# =============================================================================
# tools/coverage.sh — line/branch coverage for the host-native unit suite.
#
# Builds the tests/ suite with gcov instrumentation (--coverage), runs both
# test binaries to emit .gcda data, then reports per-file line and branch
# coverage for the kernel/port sources that are compiled on the host, plus a
# weighted total.
#
# Files compiled into more than one binary (e.g. perf.c lives in both
# vaios_tests and vaios_utils_tests) are reported at their best-covered
# binary — lcov is not required.
#
# Usage:
#   tools/coverage.sh            # table + weighted total
#   tools/coverage.sh --list     # also list each file's 0-call functions
#
# Exit code: 0 if the suite built and ran, non-zero otherwise.
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$ROOT_DIR/build_coverage"
LIST_FUNCS=0
[ "${1:-}" = "--list" ] && LIST_FUNCS=1

for bin in gcc gcov cmake; do
  command -v "$bin" >/dev/null || { echo "missing required tool: $bin" >&2; exit 2; }
done

echo "=== configuring instrumented build ($BUILD_DIR) ==="
cmake -S "$ROOT_DIR/tests" -B "$BUILD_DIR" \
      -DCMAKE_C_COMPILER=gcc \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_SYSTEM_NAME=Linux \
      -DCMAKE_C_FLAGS="-DCORTEX_M4 --coverage -O0 -g" \
      -DCMAKE_EXE_LINKER_FLAGS="--coverage" \
      --fresh >/dev/null 2>&1 || { echo "cmake configure failed" >&2; exit 1; }

echo "=== building ==="
cmake --build "$BUILD_DIR" --parallel >/dev/null 2>&1 || { echo "build failed" >&2; exit 1; }

echo "=== running test binaries (generating .gcda) ==="
"$BUILD_DIR/vaios_tests"        >/dev/null 2>&1 || echo "  warning: vaios_tests exited non-zero" >&2
"$BUILD_DIR/vaios_utils_tests"  >/dev/null 2>&1 || echo "  warning: vaios_utils_tests exited non-zero" >&2

# --- parse one gcov run for a given source path ------------------------------
# Echoes: "<linepct> <linetotal> <brpct> <brtotal>" or nothing if not found.
cov_for() {
  local gcda="$1" srcbase="$2"
  gcov -b -n -o "$(dirname "$gcda")" "$gcda" 2>/dev/null \
    | awk -v want="/$srcbase'" '
        index($0, "File ") == 1 { infile = (index($0, want) > 0) }
        infile && /Lines executed:/    { split($0, a, ":"); split(a[2], b, "% of "); lp=b[1]; lt=b[2] }
        infile && /Branches executed:/ { split($0, a, ":"); split(a[2], b, "% of "); bp=b[1]; bt=b[2]; print lp, lt, bp, bt; infile=0 }
      '
}

echo
printf "%-16s %9s %9s   %s\n" "FILE" "LINE%" "BRANCH%" "(lines covered / total)"
printf -- "---------------------------------------------------------------\n"

declare -A done_base
tot_cov=0; tot_lines=0

# Iterate only the kernel sources under test (the .gcda path mirrors the
# source tree, so '*/kernel/*' excludes the test files and host stubs).
for gcda in $(find "$BUILD_DIR" -path '*/kernel/*.c.gcda' | sort); do
  base="$(basename "$gcda" .c.gcda)"
  [ -n "${done_base[$base]:-}" ] && continue

  best_lp=0; best_lt=0; best_bp=0; best_bt=0; best_cov=-1; best_g=""
  # A source may exist in several binaries; pick the run covering the most lines.
  for g in $(find "$BUILD_DIR" -path '*/kernel/*' -name "$base.c.gcda"); do
    read -r lp lt bp bt < <(cov_for "$g" "$base.c")
    [ -z "${lp:-}" ] && continue
    cov=$(awk -v p="$lp" -v t="$lt" 'BEGIN{printf "%d", (p*t)/100 + 0.5}')
    if [ "$cov" -gt "$best_cov" ]; then
      best_cov=$cov; best_lp=$lp; best_lt=$lt; best_bp=$bp; best_bt=$bt; best_g=$g
    fi
  done
  done_base[$base]=1
  [ "$best_cov" -lt 0 ] && continue

  printf "%-16s %8s%% %8s%%   (%d / %s)\n" "$base.c" "$best_lp" "$best_bp" "$best_cov" "$best_lt"
  tot_cov=$((tot_cov + best_cov))
  tot_lines=$((tot_lines + best_lt))

  if [ "$LIST_FUNCS" -eq 1 ]; then
    g="$best_g"
    uncov=$(gcov -f -n -o "$(dirname "$g")" "$g" 2>/dev/null \
            | grep -B1 'executed:0.00%' | grep "Function '" \
            | sed "s/.*Function '/      /;s/'.*//" | tr '\n' ' ')
    [ -n "$uncov" ] && echo "      0-call: $uncov"
  fi
done

printf -- "---------------------------------------------------------------\n"
if [ "$tot_lines" -gt 0 ]; then
  pct=$(awk -v c="$tot_cov" -v t="$tot_lines" 'BEGIN{printf "%.1f", (100.0*c)/t}')
  printf "%-16s %8s%%          (%d / %d lines, host-compiled sources)\n" "TOTAL" "$pct" "$tot_cov" "$tot_lines"
fi
echo
echo "Note: port.c / port_hw.c / semihosting.c / qemu_irq.c are ARM-only and"
echo "      are NOT compiled on the host — validate those via SITL/PITL."
