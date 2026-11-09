#!/usr/bin/env bash
# tools/renode_upipe.sh — v_upipe (VAIOS_UPIPE) under Renode.
#
# Two unprivileged tasks move 8 B and 256 B messages through the zero-copy
# pipe and through a bus pipe topic, checking every payload; a third task that
# is not an end of the pipe reads it and must fault there and be killed. The
# ns/msg figures are Renode's and mean nothing: hardware has the real ones
# (tools/run_hw_tests.sh).
#
# Exit: 0 = pass (or skipped when the toolchain/renode is absent), 1 = fail.
set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build_sitl_upipe"

for bin in arm-none-eabi-gcc cmake renode; do
  command -v "$bin" >/dev/null 2>&1 || { echo "SKIP: '$bin' not installed"; exit 0; }
done

export srctree="$ROOT_DIR/extern/NavHAL"
echo "=== building upipe (NAVHAL) ==="
rm -rf "$BUILD_DIR"
if ! cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DNAVHAL=ON -DEXAMPLES=ON -DVAIOS_CONSOLE_DMA=OFF \
       -DVAIOS_MODULE_BUS=ON -DVAIOS_EXAMPLE=UPIPE >/tmp/sitl_upipe_cfg.log 2>&1 \
   || ! cmake --build "$BUILD_DIR" -j >/tmp/sitl_upipe_bld.log 2>&1; then
  echo "FAIL: build (see /tmp/sitl_upipe_bld.log)"; tail -15 /tmp/sitl_upipe_bld.log
  exit 1
fi

log="$(mktemp)"
timeout 180s renode --console --disable-xwt \
    -e "\$bin=@$BUILD_DIR/examples/main" -e '$run="6.0"' \
    -e "include @$ROOT_DIR/tools/renode.resc" </dev/null >"$log" 2>&1 || true
clean="$(sed -E 's/.*usart2: \[[^]]*\] ?//; s/\x1b\[[0-9;]*m//g' "$log")"
rm -f "$log"
echo "---- captured UART ----"
echo "$clean" | grep -aE '\[up\]|contained|PANIC' || true
echo "-----------------------"

fail=0
check() { # $1 = description, $2 = 0/1 result
  if [ "$2" -eq 1 ]; then echo "  PASS: $1"; else echo "  FAIL: $1"; fail=1; fi
}
has() { echo "$clean" | grep -aqE "$1" && echo 1 || echo 0; }

check "8 B and 256 B through both paths" \
  "$([ "$(echo "$clean" | grep -acE '\[up\] (8|256) B: upipe [0-9]+ ns/msg, bus [0-9]+ ns/msg')" -eq 2 ] && echo 1 || echo 0)"
check "every payload intact"            "$(has '\[up\] data ok')"
check "intruder faulted and was killed" "$(has 'fault in task 4 contained')"
pipe="$(echo "$clean" | grep -aoE 'pipe 0x[0-9a-f]+' | head -1 | cut -d' ' -f2)"
check "...at the pipe's address"        "$(has "\[up\] faults=1 last addr $pipe pipe $pipe")"
check "intruder never read it"          "$(has 'BUG' | tr 01 10)"
check "no kernel panic"                 "$(has 'KERNEL PANIC' | tr 01 10)"

if [ "$fail" -eq 0 ]; then
  echo "=== SITL upipe: ALL PASS ==="
  exit 0
fi
echo "=== SITL upipe: FAIL ==="
exit 1
