#!/usr/bin/env bash
# tools/renode_fdipc.sh — Stage-5 fd-typed IPC regression under Renode.
#
# Builds the stage5_fdipc image (DEVFS + IPC_FD + SVC) and runs it headless
# under the REAL scheduler + SVC path, asserting the fd-typed IPC fixes hold
# on-target:
#   #7   an over-long named-object name is rejected, not silently truncated;
#   #12  a non-recursive fd mutex re-locked by its owner returns (no self-lock);
#   #4   a task that exits with open named-sem fds has them closed by teardown,
#        so the table slots are reclaimed.
#
# Exit: 0 = pass (or skipped when the toolchain/renode is absent), 1 = fail.
set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT_DIR="$ROOT_DIR/tools"
BUILD_DIR="$ROOT_DIR/build_sitl_fdipc"

for bin in arm-none-eabi-gcc cmake renode; do
  command -v "$bin" >/dev/null 2>&1 || { echo "SKIP: '$bin' not installed"; exit 0; }
done

export srctree="$ROOT_DIR/extern/NavHAL"

echo "=== building stage5_fdipc (NAVHAL) ==="
if ! cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DNAVHAL=ON -DEXAMPLES=ON \
       -DVAIOS_EXAMPLE=STAGE5_FDIPC >/tmp/sitl_fdipc_cfg.log 2>&1 \
   || ! cmake --build "$BUILD_DIR" -j >/tmp/sitl_fdipc_bld.log 2>&1; then
  echo "FAIL: build (see /tmp/sitl_fdipc_bld.log)"; tail -15 /tmp/sitl_fdipc_bld.log
  exit 1
fi

log="$(mktemp)"
for attempt in 1 2 3; do
  : > "$log"
  timeout 120s renode --console --disable-xwt \
      -e "\$bin=@$BUILD_DIR/examples/main" \
      -e '$run="6.0"' \
      -e "include @$SCRIPT_DIR/renode.resc" </dev/null >"$log" 2>&1 || true
  grep -aq 'fdipc' "$log" && break
  echo "  renode produced no on-target output (attempt $attempt); retrying ..."
done

clean="$(sed -E 's/.*usart2: \[[^]]*\] ?//; s/\x1b\[[0-9;]*m//g' "$log")"
echo "---- captured UART ----"
echo "$clean" | grep -aE 'fdipc|PANIC|Halt|fault' || true
echo "-----------------------"
rm -f "$log"

fail=0
assert_present() { if echo "$clean" | grep -aqE "$2"; then echo "  PASS: $1"
  else echo "  FAIL: $1  (missing: $2)"; fail=1; fi; }
assert_absent() { if echo "$clean" | grep -aqE "$2"; then echo "  FAIL: $1  (saw: $2)"; fail=1
  else echo "  PASS: $1"; fi; }
assert_present "#7 over-long name rejected"        'PASS #7'
assert_present "#12 owner re-lock returned"        'PASS #12'
assert_present "#4 named-sem slots freed on exit"  'PASS #4'
assert_absent  "#7 not silently truncated"         'FAIL #7'
assert_absent  "#4 table not exhausted"            'FAIL #4'
assert_absent  "no kernel panic"                   'KERNEL PANIC|System Halted'
assert_present "runner reached the end"            '\[fdipc\] DONE'

if [ "$fail" -eq 0 ]; then
  echo "=== SITL fd-IPC: ALL PASS ==="
  exit 0
fi
echo "=== SITL fd-IPC: FAIL ==="
exit 1
