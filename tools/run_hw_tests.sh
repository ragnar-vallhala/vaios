#!/usr/bin/env bash
# =============================================================================
# tools/run_hw_tests.sh
#
# Hardware regression suite for vaios on an STM32F401RE Nucleo.
#
# For each curated example the script:
#   1. Configures and builds the firmware with -DVAIOS_EXAMPLE=<name>.
#   2. Flashes the resulting .bin to the board via st-flash.
#   3. Captures UART output from the on-board ST-Link VCP for a fixed window.
#   4. Greps the captured log for required PASS / completion lines.
#
# Exit code: number of examples that failed (0 = all green).
#
# Requirements: arm-none-eabi-gcc, arm-none-eabi-objcopy, cmake, st-flash,
#               st-info, a connected Nucleo on /dev/ttyACM0.
#
# Usage:
#   tools/run_hw_tests.sh                # default port /dev/ttyACM0
#   PORT=/dev/ttyACM1 tools/run_hw_tests.sh
#   CAPTURE_SECS=20  tools/run_hw_tests.sh
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

PORT="${PORT:-/dev/ttyACM0}"
CAPTURE_SECS="${CAPTURE_SECS:-15}"
BUILD_DIR="build_hw_tests"

# Tee per-example output into a log so the cross-suite summary table at
# the end can re-parse the same suite/test lines that the host test
# runner uses (tools/run_tests.sh). mktemp'd and rm'd on exit.
SUMMARY_LOG="$(mktemp)"
trap 'rm -f "$SUMMARY_LOG"' EXIT

c_red()   { printf '\033[1;31m%s\033[0m' "$1"; }
c_green() { printf '\033[1;32m%s\033[0m' "$1"; }
c_blue()  { printf '\033[1;34m%s\033[0m' "$1"; }

# ----- pre-flight checks ------------------------------------------------------
for bin in arm-none-eabi-gcc arm-none-eabi-objcopy cmake st-flash st-info; do
  if ! command -v "$bin" >/dev/null; then
    echo "missing required tool: $bin" >&2
    exit 2
  fi
done

if ! st-info --probe 2>&1 | grep -qi "stlink\|serial"; then
  echo "no ST-Link board detected (st-info --probe found nothing)" >&2
  exit 2
fi

if [ ! -e "$PORT" ]; then
  echo "serial port $PORT not present (set PORT=... to override)" >&2
  exit 2
fi

# ----- per-example runner -----------------------------------------------------
# Output mirrors the host-test runner (tools/run_tests.sh) so the same
# summary-table parser can chew through both. Each example becomes a
# Suite; each required UART pattern is one "test" whose assertion count
# is 1. Build / flash failures emit a synthetic single FAIL test so the
# suite still shows up in the summary.
PASSES=0
FAILS=0
FAIL_NAMES=()

# Print one host-format test line (PASS or FAIL) and mirror to SUMMARY_LOG.
#   $1 = name shown on the left  (truncated to 50 chars)
#   $2 = "PASS" or "FAIL"
#   $3 = passed-asserts count    (for PASS: total; for FAIL: passed/total)
#   $4 = total-asserts count
emit_test_line() {
  local label="$1" verdict="$2" got="$3" tot="$4"
  local short="${label:0:50}"
  if [ "$verdict" = "PASS" ]; then
    printf '  %-52s\033[32mPASS\033[0m (%d)\n' "$short" "$tot" | tee -a "$SUMMARY_LOG"
  else
    printf '  %-52s\033[31mFAIL\033[0m (%d/%d)\n' "$short" "$got" "$tot" | tee -a "$SUMMARY_LOG"
  fi
}

emit_suite_header() {
  printf '\n\033[1;34m=== Suite: %s ===\033[0m\n' "$1" | tee -a "$SUMMARY_LOG"
}
emit_suite_footer() {
  local name="$1" pass="$2" fail="$3"
  printf '\n\033[1m[%s] Results: \033[32m%d passed\033[0m, \033[31m%d failed\033[0m\n' \
    "$name" "$pass" "$fail" | tee -a "$SUMMARY_LOG"
}

run_example() {
  local name="$1"; shift
  local patterns=("$@")

  emit_suite_header "$name"

  rm -rf "$BUILD_DIR"
  mkdir "$BUILD_DIR"

  local cmake_log="/tmp/hw_${name}_cmake.log"
  local build_log="/tmp/hw_${name}_build.log"
  local uart_log="/tmp/hw_${name}_uart.log"

  if ! ( cd "$BUILD_DIR" && cmake -DNAVHAL=ON -DEXAMPLES=ON \
             -DVAIOS_EXAMPLE="$name" .. > "$cmake_log" 2>&1 ); then
    emit_test_line "build (cmake configure)" FAIL 0 1
    echo "    cmake log: $cmake_log"
    emit_suite_footer "$name" 0 1
    FAILS=$((FAILS+1)); FAIL_NAMES+=("$name(cmake)")
    return
  fi
  if ! cmake --build "$BUILD_DIR" --parallel > "$build_log" 2>&1; then
    emit_test_line "build (compile)" FAIL 0 1
    echo "    build log: $build_log"
    emit_suite_footer "$name" 0 1
    FAILS=$((FAILS+1)); FAIL_NAMES+=("$name(build)")
    return
  fi

  arm-none-eabi-objcopy -O binary \
      "$BUILD_DIR/examples/main" \
      "$BUILD_DIR/examples/main.bin"

  # st-flash occasionally fails the first SWD handshake when the previously-
  # flashed firmware has put the MCU into WFI / reconfigured the debug pins.
  # Retry once before giving up; on persistent failure dump the st-flash log
  # so the user can see the actual reason (AIRCR write failure, SWD enter
  # failure, NRST not connected, etc.) instead of an opaque "FLASH FAILED".
  local flash_log="/tmp/hw_${name}_flash.log"
  local flashed=0
  for attempt in 1 2; do
    if st-flash --connect-under-reset write \
         "$BUILD_DIR/examples/main.bin" 0x8000000 \
         > "$flash_log" 2>&1; then
      flashed=1; break
    fi
    sleep 1
  done
  if [ "$flashed" -ne 1 ]; then
    emit_test_line "flash" FAIL 0 1
    echo "    flash log: $flash_log"
    echo "    ── last 5 lines ──"
    tail -n 5 "$flash_log" | sed 's/^/    /'
    echo "    Recovery: hold the Nucleo's BLACK reset button, re-run the"
    echo "              script, release reset when st-flash starts writing."
    echo "              Or simply unplug/replug the board's USB cable."
    emit_suite_footer "$name" 0 1
    FAILS=$((FAILS+1)); FAIL_NAMES+=("$name(flash)")
    return
  fi

  # Capture UART. We start the capture, then trigger a reset so the board's
  # boot output lands inside our window.
  stty -F "$PORT" 115200 raw -echo
  : > "$uart_log"
  ( timeout "$CAPTURE_SECS" cat "$PORT" > "$uart_log" 2>/dev/null ) &
  local cap_pid=$!
  sleep 1
  st-flash reset >/dev/null 2>&1 || true
  wait "$cap_pid" || true

  local p_pass=0 p_fail=0
  for pat in "${patterns[@]}"; do
    # Strip backslash escapes for display so the user sees the human form
    # of the pattern instead of its regex source.
    local display="${pat//\\/}"
    if grep -qE -- "$pat" "$uart_log"; then
      emit_test_line "$display" PASS 1 1
      p_pass=$((p_pass+1))
    else
      emit_test_line "$display" FAIL 0 1
      p_fail=$((p_fail+1))
    fi
  done
  emit_suite_footer "$name" "$p_pass" "$p_fail"

  if [ "$p_fail" -eq 0 ]; then
    PASSES=$((PASSES+1))
  else
    echo "    UART log: $uart_log"
    FAILS=$((FAILS+1)); FAIL_NAMES+=("$name")
  fi
}

# ----- regression cases -------------------------------------------------------
# Pattern strings are extended regex (passed to `grep -E`). Escape brackets.

run_example FIFO_TEST \
  "SPSC Basic R/W: PASS" \
  "SPSC Overwrite Policy: PASS" \
  "SPSC Wrapping: PASS" \
  "MPMC Try Push/Pop: PASS" \
  "MPMC Bulk Op: PASS" \
  "Producer 1: Done" \
  "Consumer 2: Done"

run_example PRIORITY_INVERSION \
  "\[LPT\] Still working \(Priority 3\)" \
  "\[HPT\] Mutex acquired successfully"

run_example IPC_TEST \
  "Task 4: Acquired mutex!" \
  "Task 1: First lock acquired" \
  "Task 1: Second lock acquired" \
  "Task 1: Timeout waiting for binary semaphore" \
  "Task 2: Timeout waiting for binary semaphore"

# On-target (PIL) unit tests: the host suites run against the real kernel +
# port (real heap / IPC / formatter). See examples/unit_tests.c.
run_example UNIT_TESTS \
  "=== Suite: Memory Allocator ===" \
  "=== Suite: Structure" \
  "=== Suite: utils" \
  "ON-TARGET RESULT: ALL PASS"

# ----- summary ---------------------------------------------------------------
# Cross-suite table. Renderer is shared with tools/run_tests.sh —
# tools/lib/test_summary.awk owns the layout.
echo
sed -E 's/\x1B\[[0-9;]*[A-Za-z]//g' "$SUMMARY_LOG" \
  | awk -v title="HARDWARE TEST SUMMARY" -f "$SCRIPT_DIR/lib/test_summary.awk"

echo
if [ "$FAILS" -eq 0 ]; then
  c_green "=== HARDWARE TESTS PASSED ==="; echo
else
  c_red "=== HARDWARE TESTS FAILED ==="; echo
  echo "  Failed examples: ${FAIL_NAMES[*]}"
fi
exit "$FAILS"
