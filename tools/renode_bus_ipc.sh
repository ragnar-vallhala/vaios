#!/usr/bin/env bash
# tools/renode_bus_ipc.sh — Bus IPC subsystem under Renode.
#
# Builds examples/56_bus_ipc.c (NAVHAL; VAIOS_MODULE_BUS is forced on for it)
# and runs it headless on the STM32F4 model: a real TIM2 IRQ publishes onto an
# overwrite topic while fast/slow reader tasks and a drop topic run. Passes on
# the firmware's "BUS-IPC PASS".
#
# Exit: 0 = pass (or skipped when the toolchain/renode is absent), 1 = fail.
set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build_renode_bus_ipc"

for bin in arm-none-eabi-gcc cmake renode; do
  command -v "$bin" >/dev/null 2>&1 || { echo "SKIP: '$bin' not installed"; exit 0; }
done

export srctree="$ROOT_DIR/extern/NavHAL"

echo "=== building bus_ipc (NAVHAL) ==="
mkdir -p "$BUILD_DIR"
if ! cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DNAVHAL=ON -DEXAMPLES=ON \
       -DVAIOS_EXAMPLE=BUS_IPC >"$BUILD_DIR/cfg.log" 2>&1 \
   || ! cmake --build "$BUILD_DIR" -j >"$BUILD_DIR/bld.log" 2>&1; then
  echo "FAIL: build (see $BUILD_DIR/bld.log)"; tail -15 "$BUILD_DIR/bld.log"
  exit 1
fi

log="$(mktemp)"
for attempt in 1 2 3; do
  : > "$log"
  timeout 180s renode --console --disable-xwt \
      -e "\$bin=@$BUILD_DIR/examples/main" \
      -e '$run="3.0"' \
      -e "include @$ROOT_DIR/tools/renode.resc" </dev/null >"$log" 2>&1 || true
  grep -aq 'bus-ipc' "$log" && break
  echo "  renode produced no on-target output (attempt $attempt); retrying ..."
done

clean="$(sed -E 's/.*usart2: \[[^]]*\] ?//; s/\x1b\[[0-9;]*m//g' "$log")"
echo "---- captured UART ----"
echo "$clean" | grep -aE 'bus-ipc|BUS-IPC|PANIC|Halt|fault' || true
echo "-----------------------"
rm -f "$log"

if echo "$clean" | grep -aq 'BUS-IPC PASS' &&
   ! echo "$clean" | grep -aqE 'KERNEL PANIC|System Halted'; then
  echo "=== SITL bus IPC: PASS ==="; exit 0
fi
echo "=== SITL bus IPC: FAIL ==="; exit 1
