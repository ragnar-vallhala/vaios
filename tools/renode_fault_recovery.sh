#!/usr/bin/env bash
# tools/renode_fault_recovery.sh — MPU fault containment + restart under Renode.
#
# Builds the mpu_fault_recovery image (VAIOS_MPU_FAULT_RECOVERY + _RESTART) and
# runs it headless. Unprivileged tasks that fault on every run, one per fault
# policy: the default (restart, cap 3), KILL, and a spawned child with RESTART
# and a cap of 1; a PANIC child must be refused to an unprivileged parent. No
# panic, and an unrelated task keeps running.
#
# Exit: 0 = pass (or skipped when the toolchain/renode is absent), 1 = fail.
set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT_DIR="$ROOT_DIR/tools"
BUILD_DIR="$ROOT_DIR/build_sitl_fault"

for bin in arm-none-eabi-gcc cmake renode; do
  command -v "$bin" >/dev/null 2>&1 || { echo "SKIP: '$bin' not installed"; exit 0; }
done

export srctree="$ROOT_DIR/extern/NavHAL"

# build_and_run <build dir> [extra cmake args...]: sets $clean to the UART.
build_and_run() {
  local dir="$1"; shift
  echo "=== building mpu_fault_recovery (NAVHAL) $* ==="
  if ! cmake -S "$ROOT_DIR" -B "$dir" -DNAVHAL=ON -DEXAMPLES=ON -DVAIOS_CONSOLE_DMA=OFF \
         -DVAIOS_EXAMPLE=MPU_FAULT_RECOVERY "$@" >/tmp/sitl_fault_cfg.log 2>&1 \
     || ! cmake --build "$dir" -j >/tmp/sitl_fault_bld.log 2>&1; then
    echo "FAIL: build (see /tmp/sitl_fault_bld.log)"; tail -15 /tmp/sitl_fault_bld.log
    exit 1
  fi
  local log; log="$(mktemp)"
  for attempt in 1 2 3; do
    : > "$log"
    timeout 120s renode --console --disable-xwt \
        -e "\$bin=@$dir/examples/main" \
        -e '$run="4.0"' \
        -e "include @$SCRIPT_DIR/renode.resc" </dev/null >"$log" 2>&1 || true
    grep -aq '\[fr\]' "$log" && break
    echo "  renode produced no on-target output (attempt $attempt); retrying ..."
  done
  clean="$(sed -E 's/.*usart2: \[[^]]*\] ?//; s/\x1b\[[0-9;]*m//g' "$log")"
  echo "---- captured UART ----"
  echo "$clean" | grep -aE '\[fr\] (esc|over|child|priv|irq|isr|panic|faults|DONE|BUG)|contained|PANIC|MPU fault|Halt' || true
  echo "-----------------------"
  rm -f "$log"
}

build_and_run "$BUILD_DIR"

fail=0
check() { # $1 = description, $2 = 0/1 result
  if [ "$2" -eq 1 ]; then echo "  PASS: $1"; else echo "  FAIL: $1"; fail=1; fi
}
count() { echo "$clean" | grep -ac "$1"; }

check "default policy: escaper ran 4 times (1 + 3 restarts)" "$([ "$(count '\[fr\] escaper start')" -eq 4 ] && echo 1 || echo 0)"
check "KILL: overflow ran once"                "$([ "$(count '\[fr\] overflow start')" -eq 1 ] && echo 1 || echo 0)"
check "spawned RESTART cap 1: child ran twice" "$([ "$(count '\[fr\] child start')" -eq 2 ] && echo 1 || echo 0)"
check "PANIC child refused to an unprivileged parent" "$(echo "$clean" | grep -aq '\[fr\] panic child refused' && echo 1 || echo 0)"
check "kernel counted 7 faults, 4 restarts"   "$(echo "$clean" | grep -aq '\[fr\] faults=7 restarts=4' && echo 1 || echo 0)"
check "survivor still running after the report" \
  "$(echo "$clean" | sed -n '/\[fr\] DONE/,$p' | grep -aq 'survivor tick' && echo 1 || echo 0)"
check "no fault escaped the MPU"              "$(echo "$clean" | grep -aq 'BUG:' && echo 0 || echo 1)"
check "no kernel panic"                       "$(echo "$clean" | grep -aqE 'KERNEL PANIC|System Halted' && echo 0 || echo 1)"

# The same policies with VAIOS_MPU_STACK_SUBREGIONS: stacks that are not powers
# of two, each block in the top eighths of its region. Seeded through the
# NavHAL config, which the build copies into a fresh .config.
#
# Without the overflow task: Renode does not model subregion-disable (nor
# region priority on overlap), so an overflow runs on through the disabled
# eighths below its block -- into other heap blocks -- before it faults. On
# hardware those eighths fall through to the privileged-only SRAM region and
# fault at once; tools/run_hw_tests.sh covers that case.
sub_cfg="$(mktemp)"
cat "$ROOT_DIR/navhal.config" > "$sub_cfg"
echo "CONFIG_VAIOS_MPU_STACK_SUBREGIONS=y" >> "$sub_cfg"
rm -rf "${BUILD_DIR}_sub"
build_and_run "${BUILD_DIR}_sub" -DNAVHAL_CONFIG_FILE="$sub_cfg" -DFR_NO_OVERFLOW=1
rm -f "$sub_cfg"
check "subregions: escaper ran 4 times"        "$([ "$(count '\[fr\] escaper start')" -eq 4 ] && echo 1 || echo 0)"
check "subregions: child ran twice"            "$([ "$(count '\[fr\] child start')" -eq 2 ] && echo 1 || echo 0)"
check "subregions: 6 faults, 4 restarts"       "$(echo "$clean" | grep -aq '\[fr\] faults=6 restarts=4' && echo 1 || echo 0)"
check "subregions: survivor still running" \
  "$(echo "$clean" | sed -n '/\[fr\] DONE/,$p' | grep -aq 'survivor tick' && echo 1 || echo 0)"
check "subregions: no fault escaped, no panic" \
  "$(echo "$clean" | grep -aqE 'BUG:|KERNEL PANIC' && echo 0 || echo 1)"

# VAIOS_MPU_FAULT_PRIV: a privileged task (RESTART, cap 1) faults too, and is
# contained like the others; inside a critical section it must still halt.
priv_cfg="$(mktemp)"
cat "$ROOT_DIR/navhal.config" > "$priv_cfg"
echo "CONFIG_VAIOS_MPU_FAULT_PRIV=y" >> "$priv_cfg"
rm -rf "${BUILD_DIR}_priv" "${BUILD_DIR}_privcrit"
build_and_run "${BUILD_DIR}_priv" -DNAVHAL_CONFIG_FILE="$priv_cfg"
check "privileged: priv ran twice (cap 1)"     "$([ "$(count '\[fr\] priv start')" -eq 2 ] && echo 1 || echo 0)"
check "privileged: 9 faults, 5 restarts"       "$(echo "$clean" | grep -aq '\[fr\] faults=9 restarts=5' && echo 1 || echo 0)"
check "privileged: no fault escaped, no panic" \
  "$(echo "$clean" | grep -aqE 'BUG:|KERNEL PANIC' && echo 0 || echo 1)"
build_and_run "${BUILD_DIR}_privcrit" -DNAVHAL_CONFIG_FILE="$priv_cfg" -DFR_PRIV_CRIT=1
rm -f "$priv_cfg"
check "privileged in a critical section: halts" \
  "$(echo "$clean" | grep -aqE 'KERNEL PANIC' && echo "$clean" | grep -aq 'MPU fault in task' && echo 1 || echo 0)"
check "privileged in a critical section: not restarted" "$([ "$(count '\[fr\] priv start')" -eq 1 ] && echo 1 || echo 0)"

# VAIOS_MPU_FAULT_ISR: a fault in an opted-in IRQ handler is contained and the
# IRQ disabled -- once with a basic task frame, once with an extended one --
# and a handler that was not opted in still halts.
isr_cfg="$(mktemp)"
cat "$ROOT_DIR/navhal.config" > "$isr_cfg"
echo "CONFIG_VAIOS_MPU_FAULT_ISR=y" >> "$isr_cfg"
rm -rf "${BUILD_DIR}_isr" "${BUILD_DIR}_isrpanic"
build_and_run "${BUILD_DIR}_isr" -DNAVHAL_CONFIG_FILE="$isr_cfg"
check "ISR: two handler faults contained"      "$(echo "$clean" | grep -aq '\[fr\] isr_faults=2 last_irq=71' && echo 1 || echo 0)"
check "ISR: both IRQs reported failed, once"   "$(echo "$clean" | grep -aq '\[fr\] irq failed=37,71,-1' && echo 1 || echo 0)"
check "ISR: interrupted task resumed, s0 kept" "$(echo "$clean" | grep -aq '\[fr\] irqtest resumed, s0 kept' && echo 1 || echo 0)"
check "ISR: task faults unaffected (7, 4)"     "$(echo "$clean" | grep -aq '\[fr\] faults=7 restarts=4' && echo 1 || echo 0)"
check "ISR: no fault escaped, no panic" \
  "$(echo "$clean" | grep -aqE 'BUG:|KERNEL PANIC' && echo 0 || echo 1)"
build_and_run "${BUILD_DIR}_isrpanic" -DNAVHAL_CONFIG_FILE="$isr_cfg" -DFR_ISR_PANIC=1
rm -f "$isr_cfg"
check "ISR not opted in: halts" \
  "$(echo "$clean" | grep -aqE 'KERNEL PANIC' && echo "$clean" | grep -aq 'fault addr 0x0 MMFSR 0x1' && echo 1 || echo 0)"

# The PANIC policy: escaper's first fault must halt the system, unrecovered.
build_and_run "${BUILD_DIR}_panic" -DFR_PANIC=1
check "PANIC policy: escaper's first fault halts" \
  "$(echo "$clean" | grep -aqE 'KERNEL PANIC' && echo "$clean" | grep -aq 'MPU fault in task' && echo 1 || echo 0)"
check "PANIC policy: escaper not restarted"     "$([ "$(count '\[fr\] escaper start')" -eq 1 ] && echo 1 || echo 0)"
check "PANIC policy: monitor never reported"    "$(echo "$clean" | grep -aq '\[fr\] DONE' && echo 0 || echo 1)"

if [ "$fail" -eq 0 ]; then
  echo "=== SITL fault recovery: ALL PASS ==="
  exit 0
fi
echo "=== SITL fault recovery: FAIL ==="
exit 1
