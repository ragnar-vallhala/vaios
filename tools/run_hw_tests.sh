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

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

PORT="${PORT:-/dev/ttyACM0}"
CAPTURE_SECS="${CAPTURE_SECS:-15}"
BUILD_DIR="build_hw_tests"

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
PASSES=0
FAILS=0
FAIL_NAMES=()

run_example() {
  local name="$1"; shift
  # remaining args = required regex patterns

  c_blue "════════════════════════════════════════"; echo
  c_blue "  $name"; echo
  c_blue "════════════════════════════════════════"; echo

  rm -rf "$BUILD_DIR"
  mkdir "$BUILD_DIR"

  local cmake_log="/tmp/hw_${name}_cmake.log"
  local build_log="/tmp/hw_${name}_build.log"
  local uart_log="/tmp/hw_${name}_uart.log"

  if ! ( cd "$BUILD_DIR" && cmake -DNAVHAL=ON -DEXAMPLES=ON \
             -DVAIOS_EXAMPLE="$name" .. > "$cmake_log" 2>&1 ); then
    c_red "  CMAKE FAILED"; echo " — see $cmake_log"
    FAILS=$((FAILS+1)); FAIL_NAMES+=("$name(cmake)")
    return
  fi
  if ! cmake --build "$BUILD_DIR" --parallel > "$build_log" 2>&1; then
    c_red "  BUILD FAILED"; echo " — see $build_log"
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
    c_red "  FLASH FAILED"; echo " — log: $flash_log"
    echo "  ── last 5 lines ──"
    tail -n 5 "$flash_log" | sed 's/^/  /'
    echo "  ──────────────────"
    echo "  Recovery: hold the Nucleo's BLACK reset button, re-run the"
    echo "            script, release reset when st-flash starts writing."
    echo "            Or simply unplug/replug the board's USB cable."
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

  local missing=0
  for pat in "$@"; do
    if ! grep -qE -- "$pat" "$uart_log"; then
      echo "  MISSING: $pat"
      missing=1
    fi
  done

  if [ "$missing" -eq 0 ]; then
    c_green "  PASS"; echo
    PASSES=$((PASSES+1))
  else
    c_red "  FAIL"; echo " — UART log: $uart_log"
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

# ----- summary ---------------------------------------------------------------
echo
c_blue "════════════════════════════════════════"; echo
if [ "$FAILS" -eq 0 ]; then
  c_green "  Hardware regression: $PASSES pass, 0 fail"
else
  c_red "  Hardware regression: $PASSES pass, $FAILS fail"
  echo
  echo "  Failed examples: ${FAIL_NAMES[*]}"
fi
echo
c_blue "════════════════════════════════════════"; echo
exit "$FAILS"
