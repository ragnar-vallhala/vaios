#!/usr/bin/env bash
# tools/renode_fault_recovery.sh — MPU fault containment + restart under Renode.
#
# Builds the mpu_fault_recovery image (VAIOS_MPU_FAULT_RECOVERY + _RESTART) and
# runs it headless. Two unprivileged tasks fault on every run -- one reads
# kernel SRAM, one overflows its stack -- and must be ended and
# restarted VAIOS_MPU_FAULT_RESTART_MAX (3) times each, then stay ended, with
# no panic, while an unrelated task keeps running.
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

echo "=== building mpu_fault_recovery (NAVHAL) ==="
if ! cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DNAVHAL=ON -DEXAMPLES=ON -DVAIOS_CONSOLE_DMA=OFF \
       -DVAIOS_EXAMPLE=MPU_FAULT_RECOVERY >/tmp/sitl_fault_cfg.log 2>&1 \
   || ! cmake --build "$BUILD_DIR" -j >/tmp/sitl_fault_bld.log 2>&1; then
  echo "FAIL: build (see /tmp/sitl_fault_bld.log)"; tail -15 /tmp/sitl_fault_bld.log
  exit 1
fi

log="$(mktemp)"
for attempt in 1 2 3; do
  : > "$log"
  timeout 120s renode --console --disable-xwt \
      -e "\$bin=@$BUILD_DIR/examples/main" \
      -e '$run="4.0"' \
      -e "include @$SCRIPT_DIR/renode.resc" </dev/null >"$log" 2>&1 || true
  grep -aq '\[fr\]' "$log" && break
  echo "  renode produced no on-target output (attempt $attempt); retrying ..."
done

clean="$(sed -E 's/.*usart2: \[[^]]*\] ?//; s/\x1b\[[0-9;]*m//g' "$log")"
echo "---- captured UART ----"
echo "$clean" | grep -aE '\[fr\] (esc|over|faults|DONE|BUG)|contained|PANIC|Halt' || true
echo "-----------------------"
rm -f "$log"

fail=0
check() { # $1 = description, $2 = 0/1 result
  if [ "$2" -eq 1 ]; then echo "  PASS: $1"; else echo "  FAIL: $1"; fail=1; fi
}
count() { echo "$clean" | grep -ac "$1"; }

check "escaper ran 4 times (1 + 3 restarts)"  "$([ "$(count '\[fr\] escaper start')" -eq 4 ] && echo 1 || echo 0)"
check "overflow ran 4 times (1 + 3 restarts)" "$([ "$(count '\[fr\] overflow start')" -eq 4 ] && echo 1 || echo 0)"
check "kernel counted 8 faults, 6 restarts"   "$(echo "$clean" | grep -aq '\[fr\] faults=8 restarts=6' && echo 1 || echo 0)"
check "survivor still running after the report" \
  "$(echo "$clean" | sed -n '/\[fr\] DONE/,$p' | grep -aq 'survivor tick' && echo 1 || echo 0)"
check "no fault escaped the MPU"              "$(echo "$clean" | grep -aq 'BUG:' && echo 0 || echo 1)"
check "no kernel panic"                       "$(echo "$clean" | grep -aqE 'KERNEL PANIC|System Halted' && echo 0 || echo 1)"

if [ "$fail" -eq 0 ]; then
  echo "=== SITL fault recovery: ALL PASS ==="
  exit 0
fi
echo "=== SITL fault recovery: FAIL ==="
exit 1
