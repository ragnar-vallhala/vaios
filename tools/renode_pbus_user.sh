#!/usr/bin/env bash
# tools/renode_pbus_user.sh — unprivileged peripheral-bus access under Renode.
#
# Builds examples/54_pbus_user.c (MPU user separation + DEVFS + SVC) and runs it
# headless: two UNPRIVILEGED tasks do bus transfers through v_pbus_open /
# v_pbus_xfer, each blocking in SYS_pbus_wait until the TIM3 "transfer-complete"
# IRQ wakes it, and both check that kernel-memory tx/rx pointers come back as
# EFAULT instead of faulting.
#
# Exit: 0 = pass (or skipped when the toolchain/renode is absent), 1 = fail.
set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT_DIR="$ROOT_DIR/tools"
BUILD_DIR="$ROOT_DIR/build_sitl_pbus_user"

for bin in arm-none-eabi-gcc cmake renode; do
  command -v "$bin" >/dev/null 2>&1 || { echo "SKIP: '$bin' not installed"; exit 0; }
done

export srctree="$ROOT_DIR/extern/NavHAL"

echo "=== building pbus_user (NAVHAL) ==="
mkdir -p "$BUILD_DIR"
if ! cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DNAVHAL=ON -DEXAMPLES=ON \
       -DVAIOS_EXAMPLE=PBUS_USER >"$BUILD_DIR/cfg.log" 2>&1 \
   || ! cmake --build "$BUILD_DIR" -j >"$BUILD_DIR/bld.log" 2>&1; then
  echo "FAIL: build (see $BUILD_DIR/bld.log)"; tail -15 "$BUILD_DIR/bld.log"
  exit 1
fi

log="$(mktemp)"
for attempt in 1 2 3; do
  : > "$log"
  timeout 120s renode --console --disable-xwt \
      -e "\$bin=@$BUILD_DIR/examples/main" \
      -e '$run="3.0"' \
      -e "include @$SCRIPT_DIR/renode.resc" </dev/null >"$log" 2>&1 || true
  grep -aq '\[pbus\]' "$log" && break
  echo "  renode produced no on-target output (attempt $attempt); retrying ..."
done

clean="$(sed -E 's/.*usart2: \[[^]]*\] ?//; s/\x1b\[[0-9;]*m//g' "$log")"
echo "---- captured UART ----"
echo "$clean" | grep -aE '\[pbus\]|pbus_user|PANIC|Halt|fault' || true
echo "-----------------------"
rm -f "$log"

fail=0
assert_present() { if echo "$clean" | grep -aqE "$2"; then echo "  PASS: $1"
  else echo "  FAIL: $1  (missing: $2)"; fail=1; fi; }
assert_absent() { if echo "$clean" | grep -aqE "$2"; then echo "  FAIL: $1  (saw: $2)"; fail=1
  else echo "  PASS: $1"; fi; }
assert_present "task A runs unprivileged"  '\[pbus\] A nPRIV=1'
assert_present "task B runs unprivileged"  '\[pbus\] B nPRIV=1'
assert_present "task A transfers + EFAULT" '\[pbus\] A PASS'
assert_present "task B transfers + EFAULT" '\[pbus\] B PASS'
assert_absent  "no task failed"            '\[pbus\] [AB] FAIL'
assert_absent  "no kernel panic / fault"   'KERNEL PANIC|System Halted|MPU fault'

if [ "$fail" -eq 0 ]; then
  echo "=== SITL pbus user access: ALL PASS ==="
  exit 0
fi
echo "=== SITL pbus user access: FAIL ==="
exit 1
