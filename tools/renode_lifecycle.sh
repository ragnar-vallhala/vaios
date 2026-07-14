#!/usr/bin/env bash
# tools/renode_lifecycle.sh — Stage-5 task-teardown regression under Renode.
#
# Builds the stage5_lifecycle image and runs it headless under the REAL
# scheduler, asserting the task-teardown fixes hold on-target (the host suite
# validates them with a frozen scheduler; this exercises the actual
# block/exit/wake/context-switch paths on ARM):
#   #6  a mutex held by a task that exits is reclaimed by another task;
#   #5  a task force-terminated while blocked is unlinked from the wait queue,
#       so a later give does NOT wake its dead TCB.
#
# Exit: 0 = pass (or skipped when the toolchain/renode is absent), 1 = fail.
set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT_DIR="$ROOT_DIR/tools"
BUILD_DIR="$ROOT_DIR/build_sitl_lifecycle"

for bin in arm-none-eabi-gcc cmake renode; do
  command -v "$bin" >/dev/null 2>&1 || { echo "SKIP: '$bin' not installed"; exit 0; }
done

export srctree="$ROOT_DIR/extern/NavHAL"

echo "=== building stage5_lifecycle (NAVHAL) ==="
if ! cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DNAVHAL=ON -DEXAMPLES=ON \
       -DVAIOS_EXAMPLE=STAGE5_LIFECYCLE >/tmp/sitl_life_cfg.log 2>&1 \
   || ! cmake --build "$BUILD_DIR" -j >/tmp/sitl_life_bld.log 2>&1; then
  echo "FAIL: build (see /tmp/sitl_life_bld.log)"; tail -15 /tmp/sitl_life_bld.log
  exit 1
fi

log="$(mktemp)"
for attempt in 1 2 3; do
  : > "$log"
  timeout 120s renode --console --disable-xwt \
      -e "\$bin=@$BUILD_DIR/examples/main" \
      -e '$run="4.0"' \
      -e "include @$SCRIPT_DIR/renode.resc" </dev/null >"$log" 2>&1 || true
  grep -aq 'stage5' "$log" && break
  echo "  renode produced no on-target output (attempt $attempt); retrying ..."
done

clean="$(sed -E 's/.*usart2: \[[^]]*\] ?//; s/\x1b\[[0-9;]*m//g' "$log")"
echo "---- captured UART ----"
echo "$clean" | grep -aE 'stage5|PANIC|Halt|fault' || true
echo "-----------------------"
rm -f "$log"

fail=0
assert_present() { # $1 = description, $2 = extended-regex the UART must contain
  if echo "$clean" | grep -aqE "$2"; then echo "  PASS: $1"
  else echo "  FAIL: $1  (missing: $2)"; fail=1; fi
}
assert_absent() { # $1 = description, $2 = regex the UART must NOT contain
  if echo "$clean" | grep -aqE "$2"; then echo "  FAIL: $1  (saw: $2)"; fail=1
  else echo "  PASS: $1"; fi
}
assert_present "#6 mutex reclaimed after owner exit" 'PASS #6: mutex reclaimed'
assert_present "#5 blocked-task termination stable"  'PASS #5: blocked-task termination stable'
assert_absent  "#6 mutex not leaked"                 'FAIL #6'
assert_absent  "#5 give did not wake the dead task"  'blocker woke UNEXPECTEDLY'
assert_absent  "no kernel panic"                     'KERNEL PANIC|System Halted'
assert_present "runner reached the end"              '\[stage5\] DONE'

if [ "$fail" -eq 0 ]; then
  echo "=== SITL lifecycle: ALL PASS ==="
  exit 0
fi
echo "=== SITL lifecycle: FAIL ==="
exit 1
