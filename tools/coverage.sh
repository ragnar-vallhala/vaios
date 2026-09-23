#!/usr/bin/env bash
# =============================================================================
# tools/coverage.sh — line/branch coverage for the host-native unit suite.
#
# Builds tests/ with gcov instrumentation (--coverage), runs EVERY test binary
# the suite produces, then reports per-file line/branch/function coverage for
# the kernel and port sources compiled on the host, plus a weighted total.
#
# A source is usually compiled into several binaries (different module configs:
# bus.c in vaios_tests, syscall.c in vaios_syscall_tests and vaios_ipcfd_tests,
# ...). With gcovr installed those runs are MERGED, which is the honest number.
# Without it, the fallback reports each file at its best-covered binary, which
# understates anything covered across several (that is what this script did
# before, and why ipc.c read 82% when the merged figure is 63%).
#
# Usage:
#   tools/coverage.sh                 # table + weighted total
#   tools/coverage.sh --list          # also list each file's 0-call functions
#   tools/coverage.sh --min-lines 70  # fail if line coverage drops below 70%
#
# Exit code: 0 if the suite built and ran (and --min-lines, when given, is
# met), non-zero otherwise. run_all_tests.sh reports the TOTAL line; CI passes
# --min-lines so a drop fails the build.
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$ROOT_DIR/build_coverage"
LIST_FUNCS=0
MIN_LINES=""
while [ $# -gt 0 ]; do
  case "$1" in
    --list) LIST_FUNCS=1 ;;
    --min-lines) MIN_LINES="${2:-}"; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

for bin in gcc gcov cmake; do
  command -v "$bin" >/dev/null || { echo "missing required tool: $bin" >&2; exit 2; }
done

echo "=== configuring instrumented build ($BUILD_DIR) ==="
cmake -S "$ROOT_DIR/tests" -B "$BUILD_DIR" \
      -DCMAKE_C_COMPILER=gcc \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_SYSTEM_NAME=Linux \
      -DCMAKE_C_FLAGS="--coverage -O0 -g" \
      -DCMAKE_EXE_LINKER_FLAGS="--coverage" \
      --fresh >/dev/null 2>&1 || { echo "cmake configure failed" >&2; exit 1; }

echo "=== building ==="
cmake --build "$BUILD_DIR" --parallel >/dev/null 2>&1 || { echo "build failed" >&2; exit 1; }

# Every executable the suite builds, so a binary added to tests/CMakeLists.txt
# is picked up here without editing this script.
echo "=== running test binaries (generating .gcda) ==="
ran=0
for t in "$BUILD_DIR"/vaios_*tests; do
  [ -x "$t" ] || continue
  ran=$((ran + 1))
  "$t" >/dev/null 2>&1 || echo "  warning: $(basename "$t") exited non-zero" >&2
done
[ "$ran" -gt 0 ] || { echo "no test binaries found in $BUILD_DIR" >&2; exit 1; }
echo "    $ran binaries"

REPORT="$(mktemp)"
trap 'rm -f "$REPORT"' EXIT

echo
{
if command -v gcovr >/dev/null; then
  # Merged across binaries (the accurate view).
  printf "%-28s %9s %9s %9s\n" "FILE" "LINE%" "BRANCH%" "FUNC%"
  printf -- "-----------------------------------------------------------------\n"
  gcovr --root "$ROOT_DIR" --filter 'kernel/' --filter 'portable/' \
        --exclude 'tests/' --csv 2>/dev/null \
    | awk -F, 'NR > 1 && $2 > 0 {
        printf "%-28s %8.1f%% %8.1f%% %8.1f%%\n", $1, $4*100, $7*100, $10*100
        lc += $3; lt += $2; bc += $6; bt += $5; fc += $9; ft += $8
      }
      END {
        printf "%s\n", "-----------------------------------------------------------------"
        printf "%-28s %8.1f%% %8.1f%% %8.1f%%\n", "ALL FILES (merged)", \
               lt ? 100*lc/lt : 0, bt ? 100*bc/bt : 0, ft ? 100*fc/ft : 0
        printf "TOTAL %.1f%% (%d / %d lines, %d / %d branches, host-compiled sources)\n", \
               lt ? 100*lc/lt : 0, lc, lt, bc, bt
      }'
else
  echo "(gcovr not installed: per-file numbers fall back to the best-covered"
  echo " binary per file. Binaries compile different #if configs, so a file's"
  echo " line total differs between them and totals here are approximate.)"
  echo
  # --- parse one gcov run for a given source path ----------------------------
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

  printf "%-16s %9s %9s   %s\n" "FILE" "LINE%" "BRANCH%" "(lines covered / total)"
  printf -- "---------------------------------------------------------------\n"

  declare -A done_base
  tot_cov=0; tot_lines=0

  # Kernel AND port sources under test; the .gcda path mirrors the source tree,
  # so these two patterns exclude the test files and host stubs.
  for gcda in $(find "$BUILD_DIR" \( -path '*/kernel/*.c.gcda' -o -path '*/portable/*.c.gcda' \) | sort); do
    base="$(basename "$gcda" .c.gcda)"
    [ -n "${done_base[$base]:-}" ] && continue

    best_lp=0; best_lt=0; best_bp=0; best_bt=0; best_cov=-1; best_g=""
    for g in $(find "$BUILD_DIR" \( -path '*/kernel/*' -o -path '*/portable/*' \) -name "$base.c.gcda"); do
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
  done

  printf -- "---------------------------------------------------------------\n"
  if [ "$tot_lines" -gt 0 ]; then
    pct=$(awk -v c="$tot_cov" -v t="$tot_lines" 'BEGIN{printf "%.1f", (100.0*c)/t}')
    printf "TOTAL %s%% (%d / %d lines, host-compiled sources, best-binary)\n" "$pct" "$tot_cov" "$tot_lines"
  fi
fi
} | tee "$REPORT"

# 0-call functions, per file, from whichever binary covers it best.
if [ "$LIST_FUNCS" -eq 1 ]; then
  echo
  echo "=== functions never called ==="
  declare -A seen
  for gcda in $(find "$BUILD_DIR" \( -path '*/kernel/*.c.gcda' -o -path '*/portable/*.c.gcda' \) | sort); do
    base="$(basename "$gcda" .c.gcda)"
    [ -n "${seen[$base]:-}" ] && continue
    seen[$base]=1
    uncov=""
    for g in $(find "$BUILD_DIR" \( -path '*/kernel/*' -o -path '*/portable/*' \) -name "$base.c.gcda"); do
      # gcov -f prints each "Function 'x'" block BEFORE the File line, so the
      # name is carried forward to its "Lines executed:" line.
      here=$(gcov -f -n -o "$(dirname "$g")" "$g" 2>/dev/null \
             | awk '/^Function/{fn=$2; next} /^Lines executed:0.00%/{if (fn != "") print fn; fn=""}' \
             | tr -d "'")
      uncov="$uncov $here"
    done
    # A function counts as dead only if no binary ever called it.
    dead=""
    for fn in $(echo "$uncov" | tr ' ' '\n' | sort -u); do
      [ -z "$fn" ] && continue
      n=$(echo "$uncov" | tr ' ' '\n' | grep -cx "$fn")
      total=$(find "$BUILD_DIR" \( -path '*/kernel/*' -o -path '*/portable/*' \) -name "$base.c.gcda" | wc -l)
      [ "$n" -eq "$total" ] && dead="$dead $fn"
    done
    [ -n "$dead" ] && printf "  %-16s%s\n" "$base.c" "$dead"
  done
fi

echo
echo "Note: port.c / port_hw.c / semihosting.c / qemu_irq.c / the host port are"
echo "      NOT compiled into the host suite — validate those via SITL/PITL"
echo "      (tools/run_all_tests.sh sitl) or the on-target gcov build."

# Floor check (CI). Compares the TOTAL line's line-coverage percentage.
if [ -n "$MIN_LINES" ]; then
  pct="$(awk '/^TOTAL /{print $2+0; exit}' "$REPORT")"
  echo
  if [ -z "$pct" ]; then
    echo "coverage: could not read the TOTAL line" >&2
    exit 1
  fi
  if awk -v p="$pct" -v m="$MIN_LINES" 'BEGIN{exit !(p + 0 < m + 0)}'; then
    echo "FAIL: line coverage ${pct}% is below the ${MIN_LINES}% floor" >&2
    exit 1
  fi
  echo "PASS: line coverage ${pct}% >= ${MIN_LINES}% floor"
fi
