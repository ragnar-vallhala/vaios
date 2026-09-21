#!/usr/bin/env bash
# tools/renode_bus.sh — bus arbiter heavy-contention run under Renode.
#
# Builds examples/53_bus_contention.c (NAVHAL) and runs it headless on the
# STM32F4 model: real TIM2/TIM3 IRQs and DMA2 drive the bus arbiter while
# cyclic, async and sync users contend. Passes on the firmware's "BUS PASS".
#
# Exit: 0 = pass (or skipped when the toolchain/renode is absent), 1 = fail.
set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build_renode_bus"

for bin in arm-none-eabi-gcc cmake renode; do
  command -v "$bin" >/dev/null 2>&1 || { echo "SKIP: '$bin' not installed"; exit 0; }
done

export srctree="$ROOT_DIR/extern/NavHAL"

echo "=== building bus_contention (NAVHAL) ==="
mkdir -p "$BUILD_DIR"
if ! cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DNAVHAL=ON -DEXAMPLES=ON \
       -DVAIOS_EXAMPLE=BUS_CONTENTION >"$BUILD_DIR/cfg.log" 2>&1 \
   || ! cmake --build "$BUILD_DIR" -j >"$BUILD_DIR/bld.log" 2>&1; then
  echo "FAIL: build (see $BUILD_DIR/bld.log)"; tail -15 "$BUILD_DIR/bld.log"
  exit 1
fi

log="$(mktemp)"
timeout 300s renode --console --disable-xwt \
    -e "\$bin=@$BUILD_DIR/examples/main" \
    -e '$run="4.0"' \
    -e "include @$ROOT_DIR/tools/renode.resc" </dev/null >"$log" 2>&1 || true

clean="$(sed -E 's/.*usart2: \[[^]]*\] ?//; s/\x1b\[[0-9;]*m//g' "$log")"
echo "---- captured UART ----"
echo "$clean" | grep -aE 'bus |  [a-z0-9]+(\(prio [0-9]\))?: |BUS |starved|PANIC|fault' || true
echo "-----------------------"
rm -f "$log"

if echo "$clean" | grep -aq 'BUS PASS'; then echo "PASS"; exit 0; fi
echo "FAIL"; exit 1
