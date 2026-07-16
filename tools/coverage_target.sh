#!/usr/bin/env bash
# =============================================================================
# tools/coverage_target.sh — on-target (Renode) gcov coverage for the ARM-only
# sources that the host suite (tools/coverage.sh) structurally cannot reach:
# portable/cortex-m4/{port,port_hw,semihosting,syscall}.c. Those are compiled
# only for the target, so host gcov reports them at 0% by omission.
#
# Flow (see docs and portable/cortex-m4/gcov_dump.c):
#   1. build the on-target unit runner with -DVAIOS_GCOV=ON (--coverage)
#   2. run it in Renode; at test end the image streams each TU's .gcda as
#      framed base64 over the UART
#   3. reconstruct the real .gcda files on the host (tools/gcov_uart_decode.py)
#   4. run gcov on the ARM objects and print a per-file line/branch table
#
# Renode 1.16.1 has no ARM semihosting and the NAVHAL enforcement code only runs
# under Renode, so the .gcda travel over UART rather than a host file — the
# result is byte-identical gcov data either way.
#
# Skips cleanly (exit 0) when arm-gcc / renode are absent. Exit 1 on a real
# failure (build, no ALL PASS, no frames decoded).
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$ROOT_DIR/build_cov_target"
RUN_SECS="${RUN_SECS:-8.0}"
cd "$ROOT_DIR"

for bin in arm-none-eabi-gcc arm-none-eabi-gcov cmake renode python3; do
  command -v "$bin" >/dev/null 2>&1 || { echo "SKIP: '$bin' not installed"; exit 0; }
done

export srctree="${srctree:-$ROOT_DIR/extern/NavHAL}"

echo "=== building on-target coverage image (VAIOS_GCOV=ON) ==="
if ! cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DNAVHAL=ON -DEXAMPLES=ON \
       -DVAIOS_EXAMPLE=UNIT_TESTS -DVAIOS_GCOV=ON >/tmp/covtgt_cfg.log 2>&1 \
   || ! cmake --build "$BUILD_DIR" -j"$(nproc)" >/tmp/covtgt_bld.log 2>&1; then
  echo "FAIL: build error"
  tail -n 20 /tmp/covtgt_cfg.log /tmp/covtgt_bld.log | sed 's/^/  | /'
  exit 1
fi

echo "=== running in Renode (capturing UART) ==="
UART_LOG="$(mktemp)"
for attempt in 1 2 3; do
  : > "$UART_LOG"
  timeout 180s renode --console --disable-xwt \
      -e "\$bin=@$BUILD_DIR/examples/main" \
      -e "\$run=\"$RUN_SECS\"" \
      -e "include @$SCRIPT_DIR/renode.resc" </dev/null >"$UART_LOG" 2>&1 || true
  grep -aq 'VAIOS_GCDA_DUMP_END' "$UART_LOG" && break
  echo "  no complete dump yet (attempt $attempt); retrying ..."
done

# Clean UART capture (strip ANSI + Renode's per-line "usart2: [ts]" prefix) for
# the human-readable assertions.
CLEAN="$(sed -E 's/\x1b\[[0-9;]*m//g; s/.*usart2: \[[^]]*\] ?//' "$UART_LOG")"
result="$(echo "$CLEAN" | grep -aoE 'ON-TARGET RESULT: [A-Z]+ ?[A-Z]*' | head -1)"
echo "  suite: ${result:-<no result captured>}"
case "$result" in
  *"ALL PASS"*) : ;;
  *)
    echo "FAIL: on-target suite did not report ALL PASS"
    echo "$CLEAN" | grep -aE 'Suite:|Results:|ON-TARGET' | tail -n 20 | sed 's/^/  | /'
    rm -f "$UART_LOG"; exit 1
    ;;
esac

echo "=== reconstructing .gcda from UART capture ==="
if ! python3 "$SCRIPT_DIR/gcov_uart_decode.py" "$UART_LOG" --quiet; then
  echo "FAIL: could not reconstruct .gcda from the capture"
  rm -f "$UART_LOG"; exit 1
fi
rm -f "$UART_LOG"

# --- report gcov for the ARM-only sources ------------------------------------
# One gcov run per .gcda; echo "<linepct> <linetotal> <brpct> <brtotal>".
cov_for() {
  local gcda="$1" srcbase="$2"
  arm-none-eabi-gcov -b -n -o "$(dirname "$gcda")" "$gcda" 2>/dev/null \
    | awk -v want="/$srcbase'" '
        index($0, "File ") == 1 { infile = (index($0, want) > 0) }
        infile && /Lines executed:/    { split($0, a, ":"); split(a[2], b, "% of "); lp=b[1]; lt=b[2] }
        infile && /Branches executed:/ { split($0, a, ":"); split(a[2], b, "% of "); bp=b[1]; bt=b[2]; print lp, lt, bp, bt; infile=0 }
      '
}

PORT_DIR="$BUILD_DIR/portable/CMakeFiles/portable.dir/cortex-m4"
echo
echo "on-target coverage — ARM-only sources (host gcov cannot reach these):"
printf "%-16s %9s %9s   %s\n" "FILE" "LINE%" "BRANCH%" "(lines covered / total)"
printf -- "---------------------------------------------------------------\n"
tot_cov=0; tot_lines=0
for f in port port_hw semihosting syscall qemu_irq; do
  gcda="$PORT_DIR/$f.c.gcda"
  if [ ! -f "$gcda" ]; then
    printf "%-16s %9s %9s   %s\n" "$f.c" "-" "-" "(not linked in this runner)"
    continue
  fi
  read -r lp lt bp bt < <(cov_for "$gcda" "$f.c")
  [ -z "${lp:-}" ] && { printf "%-16s %9s %9s   %s\n" "$f.c" "?" "?" "(gcov parse failed)"; continue; }
  cov=$(awk -v p="$lp" -v t="$lt" 'BEGIN{printf "%d", (p*t)/100 + 0.5}')
  printf "%-16s %8s%% %8s%%   (%d / %s)\n" "$f.c" "$lp" "$bp" "$cov" "$lt"
  tot_cov=$((tot_cov + cov)); tot_lines=$((tot_lines + lt))
done
printf -- "---------------------------------------------------------------\n"
if [ "$tot_lines" -gt 0 ]; then
  pct=$(awk -v c="$tot_cov" -v t="$tot_lines" 'BEGIN{printf "%.1f", (100.0*c)/t}')
  printf "%-16s %8s%%          (%d / %d lines, on-target ARM sources)\n" "TOTAL" "$pct" "$tot_cov" "$tot_lines"
fi
echo
echo "Note: this runner boots the kernel + port and runs the no-scheduler unit"
echo "      suites, so it covers port_hw.c bring-up. Files that no runner path"
echo "      references (semihosting.c on NAVHAL, syscall.c without SVC syscalls)"
echo "      are not linked, and port.c's scheduler/exception code needs a running"
echo "      scheduler to register — see examples/98_coverage_sched.c, the on-"
echo "      hardware complement (Renode is too slow to run an instrumented"
echo "      scheduler image, so it is not part of this Renode/CI path)."