#!/usr/bin/env bash
# =============================================================================
# tools/run_hw_tests.sh
#
# Hardware regression suite for vaios on an STM32F401RE Nucleo.
#
# For each curated example the script:
#   1. Configures and builds the firmware with -DVAIOS_EXAMPLE=<name>.
#   2. Flashes the resulting ELF to the board via OpenOCD.
#   3. Captures UART output from the on-board ST-Link VCP for a fixed window.
#   4. Greps the captured log for required PASS / completion lines.
#
# Exit code: number of examples that failed (0 = all green).
#
# 77 (the conventional skip code) with a SKIP line when there is nothing to test
# against — no toolchain, no
# probe attached, or a probe with no serial console. An absent capability is not
# a failure, the same way a missing renode is not one to renode_*.sh, and vtest's
# `check` adapter has only the exit status to go on.
#
# Still a hard failure (exit 2) when hardware IS present but wrong: a device id
# that does not match what the image was built for is a setup mistake worth
# seeing in red, not something to pass over quietly.
#
# Requirements: arm-none-eabi-gcc, cmake, openocd, and a connected board with a
#               serial console. The NAVIXSM-F401RE has no VCP, so this suite
#               cannot run against it — read the kernel log over SWD instead
#               (tools/pitl_run.sh --kmsg).
#
# Usage:
#   tools/run_hw_tests.sh                # console of the probe at USB_LOC
#   PORT=/dev/ttyACM1 tools/run_hw_tests.sh
#   USB_LOC=3-2 tools/run_hw_tests.sh          # which probe, by USB port
#   CAPTURE_SECS=20  tools/run_hw_tests.sh
#   NAVHAL_CONFIG_FILE=f767.config EXPECT_CHIPID=0x451 USB_LOC=1-1.3 \
#     tools/run_hw_tests.sh                  # another board than navhal.config's
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

# NavHAL's Kconfig globs its arch tree relative to $srctree, so a NAVHAL=ON
# configure needs it. Every renode_*.sh exports it and tools/vtest.sh exports it
# for the suites it launches; this script did neither, so it worked under vtest
# and failed when run directly. Set it here so it stands on its own.
export srctree="${srctree:-$REPO_ROOT/extern/NavHAL}"
# shellcheck source=lib/probe.sh
. "$SCRIPT_DIR/lib/probe.sh"

# Which board. USB_LOC names the probe by the port it is plugged into; see
# tools/lib/probe.sh for why a serial cannot be trusted to do that here.
PORT="${PORT:-}"
EXPECT_CHIPID="${EXPECT_CHIPID:-0x433}" # STM32F401xD/xE, what the build targets
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
for bin in arm-none-eabi-gcc cmake; do
  if ! command -v "$bin" >/dev/null; then
    echo "SKIP: missing required tool: $bin"
    exit 77
  fi
done

probe_require_tools || exit 2
if ! probe_present; then
  echo "SKIP: no ST-Link at USB location $USB_LOC (set USB_LOC=<bus>-<port>)"
  exit 77
fi

# --- check the board is the one this firmware is built for -------------------
# Writing an F401 image to an F767 is what "Failed to parse flash type" means.
# The target script has to match the family first: probe_chipid through the
# wrong one reads nothing at all, which would report as "no device id" rather
# than as the mismatch it is.
probe_target_from_build "$BUILD_DIR"
chipid=$(probe_chipid)
if [ -z "$chipid" ]; then
  echo "could not read a device id from the probe at $USB_LOC" >&2
  exit 2
fi
if [ "$chipid" != "$EXPECT_CHIPID" ]; then
  echo "the board at $USB_LOC is chipid $chipid, but this firmware targets $EXPECT_CHIPID." >&2
  echo "Refusing to flash it. Set EXPECT_CHIPID=$chipid if that is really the" >&2
  echo "board you want (and make sure navhal.config matches)." >&2
  exit 2
fi

# The serial port belonging to THIS probe, unless PORT says otherwise.
#
# It used to take the first /dev/serial/by-id entry on the machine, which is only
# right when exactly one board is attached. With two it silently flashed the
# board at USB_LOC and read the OTHER one's console: every assertion then failed
# as "missing", which looks like a dead image rather than a crossed cable.
# The tty is a child of the probe's own USB device, so ask sysfs.
if [ -z "$PORT" ]; then
  tty=$(find /sys/bus/usb/devices/"$USB_LOC":*/ -maxdepth 2 -name 'ttyACM*' \
        -printf '%f\n' 2>/dev/null | head -1)
  [ -n "$tty" ] && PORT="/dev/$tty"
fi
if [ -z "$PORT" ] || [ ! -e "$PORT" ]; then
  echo "SKIP: no serial console on the probe at USB location $USB_LOC."
  # Name the boards that COULD run this, but do not pick one. Choosing a board to
  # flash is the operator's call: the probes with a console here are development
  # boards that hold whatever was last put on them, and this suite overwrites it.
  cands=$(probe_consoles)
  if [ -n "$cands" ]; then
    echo
    echo "  These attached ST-LINKs do have one — pass USB_LOC to use one:"
    echo "$cands" | while read -r loc tty; do
      echo "      USB_LOC=$loc tools/run_hw_tests.sh      # console /dev/$tty"
    done
    echo
    echo "  Note this ERASES the board at that location."
    echo
  fi
  echo "  This suite reads the board's UART, so it needs one. The NAVIXSM-F401RE"
  echo "  has no VCP at all (its ST-Link is a clone without one) -- for that board"
  echo "  read the kernel log over SWD instead:"
  echo "      tools/pitl_run.sh --kmsg <build-dir>"
  echo "  Otherwise point USB_LOC at a probe that has a VCP, or set PORT=..."
  exit 77
fi
echo "board: probe at $USB_LOC (chipid $chipid) on $PORT"

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

  emit_suite_header "$name${EXTRA_CMAKE:+ $EXTRA_CMAKE}${LABEL:+ ($LABEL)}"

  rm -rf "$BUILD_DIR"
  mkdir "$BUILD_DIR"

  local cmake_log="/tmp/hw_${name}_cmake.log"
  local build_log="/tmp/hw_${name}_build.log"
  local uart_log="/tmp/hw_${name}_uart.log"

  # NAVHAL_CONFIG_FILE picks the board when it is not navhal.config's;
  # EXTRA_CMAKE adds per-run options (an example's variant); LABEL only names
  # the run in the report.
  # shellcheck disable=SC2086
  if ! ( cd "$BUILD_DIR" && cmake -DNAVHAL=ON -DEXAMPLES=ON \
             ${NAVHAL_CONFIG_FILE:+-DNAVHAL_CONFIG_FILE="$NAVHAL_CONFIG_FILE"} \
             ${EXTRA_CMAKE:-} -DVAIOS_EXAMPLE="$name" .. > "$cmake_log" 2>&1 ); then
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

  # The first SWD handshake can fail when the previously-flashed firmware has
  # put the MCU into WFI / reconfigured the debug pins. Retry once before giving
  # up; on persistent failure dump the log so the user can see the actual reason
  # instead of an opaque "FLASH FAILED".
  local flash_log="/tmp/hw_${name}_flash.log"
  local flashed=0
  for attempt in 1 2; do
    if probe_flash "$BUILD_DIR/examples/main" "$flash_log"; then
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
    echo "              script, release reset when writing starts."
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
  probe_reset || true
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
# port (real heap / IPC / formatter). See examples/99_unit_tests.c.
run_example UNIT_TESTS \
  "=== Suite: Memory Allocator ===" \
  "=== Suite: Structure" \
  "=== Suite: utils" \
  "ON-TARGET RESULT: ALL PASS"

# MPU fault recovery: one task per fault policy (see examples/36_mpu_fault_recovery.c).
run_example MPU_FAULT_RECOVERY \
  "\[fr\] panic child refused" \
  "\[fr\] faults=7 restarts=4" \
  "\[fr\] DONE" \
  "\[fr\] survivor tick"

# ...the same with VAIOS_MPU_STACK_SUBREGIONS (stacks that are not powers of
# two, above disabled subregions), overflow included: only hardware enforces
# subregion-disable, so this is where that case is checked.
sub_cfg="$(mktemp)"
cat "${NAVHAL_CONFIG_FILE:-$REPO_ROOT/navhal.config}" > "$sub_cfg"
echo "CONFIG_VAIOS_MPU_STACK_SUBREGIONS=y" >> "$sub_cfg"
NAVHAL_CONFIG_FILE="$sub_cfg" LABEL="subregions" run_example MPU_FAULT_RECOVERY \
  "\[fr\] panic child refused" \
  "\[fr\] faults=7 restarts=4" \
  "\[fr\] DONE" \
  "\[fr\] survivor tick"
rm -f "$sub_cfg"

# ...with VAIOS_MPU_FAULT_PRIV: a privileged task's fault is contained too.
priv_cfg="$(mktemp)"
cat "${NAVHAL_CONFIG_FILE:-$REPO_ROOT/navhal.config}" > "$priv_cfg"
echo "CONFIG_VAIOS_MPU_FAULT_PRIV=y" >> "$priv_cfg"
NAVHAL_CONFIG_FILE="$priv_cfg" LABEL="privileged" run_example MPU_FAULT_RECOVERY \
  "\[fr\] faults=9 restarts=5" \
  "\[fr\] DONE" \
  "\[fr\] survivor tick"
# ...but not inside a critical section: that still stops the system (the
# privileged task's fault is the one with IACCVIOL, MMFSR 0x1).
NAVHAL_CONFIG_FILE="$priv_cfg" LABEL="privileged, critical section" \
  EXTRA_CMAKE="-DFR_PRIV_CRIT=1" run_example MPU_FAULT_RECOVERY \
  "KERNEL PANIC" \
  "fault addr 0x0 MMFSR 0x1"
rm -f "$priv_cfg"

# ...with VAIOS_MPU_FAULT_ISR: faults in two opted-in IRQ handlers (one with
# the interrupted task's FP state live) are contained and the IRQs disabled;
# one not opted in still stops the system.
isr_cfg="$(mktemp)"
cat "${NAVHAL_CONFIG_FILE:-$REPO_ROOT/navhal.config}" > "$isr_cfg"
echo "CONFIG_VAIOS_MPU_FAULT_ISR=y" >> "$isr_cfg"
NAVHAL_CONFIG_FILE="$isr_cfg" LABEL="ISR" run_example MPU_FAULT_RECOVERY \
  "\[fr\] isr_faults=2 last_irq=71" \
  "\[fr\] irq failed=37,71,-1" \
  "\[fr\] irqtest resumed, s0 kept" \
  "\[fr\] faults=7 restarts=4" \
  "\[fr\] survivor tick"
NAVHAL_CONFIG_FILE="$isr_cfg" LABEL="ISR not opted in" \
  EXTRA_CMAKE="-DFR_ISR_PANIC=1" run_example MPU_FAULT_RECOVERY \
  "KERNEL PANIC" \
  "fault addr 0x0 MMFSR 0x1"
rm -f "$isr_cfg"

# ...with VAIOS_TASK_CHECKPOINT: escaper's run count survives its restarts.
ck_cfg="$(mktemp)"
cat "${NAVHAL_CONFIG_FILE:-$REPO_ROOT/navhal.config}" > "$ck_cfg"
echo "CONFIG_VAIOS_TASK_CHECKPOINT=y" >> "$ck_cfg"
NAVHAL_CONFIG_FILE="$ck_cfg" LABEL="checkpoint" run_example MPU_FAULT_RECOVERY \
  "\[fr\] escaper run 4" \
  "\[fr\] faults=7 restarts=4" \
  "\[fr\] survivor tick"
rm -f "$ck_cfg"

# ...and a task whose policy is PANIC: its first fault stops the system.
EXTRA_CMAKE="-DFR_PANIC=1" run_example MPU_FAULT_RECOVERY \
  "KERNEL PANIC" \
  "MPU fault in task 2"

# MMIO windows: an unprivileged task drives GPIOA (the LED) through its
# window, faults on RCC and is restarted with the window kept; an ungranted
# task faults on GPIOA (see examples/38_mmio_user.c).
run_example MMIO_USER \
  "\[mm\] gpio ok" \
  "contained \| addr 0x40023830" \
  "contained \| addr 0x40020014" \
  "\[mm\] faults=3 restarts=1"

# v_upipe: two unprivileged tasks through the zero-copy pipe and, for
# comparison, a bus pipe topic; a third task touching the pipe is killed. Last,
# because -DVAIOS_MODULE_BUS=ON stays in the build dir's cache. The ns/msg lines
# are the measurement (see examples/37_upipe.c).
EXTRA_CMAKE="-DVAIOS_MODULE_BUS=ON" run_example UPIPE \
  "fault in task 4 contained" \
  "\[up\] 8 B: upipe [0-9]+ ns/msg, bus [0-9]+ ns/msg" \
  "\[up\] 256 B: upipe [0-9]+ ns/msg, bus [0-9]+ ns/msg" \
  "\[up\] data ok" \
  "\[up\] faults=1 last addr"

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
