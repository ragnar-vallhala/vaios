#!/usr/bin/env bash
# tools/renode_mmio_user.sh — MMIO windows for unprivileged tasks
# (VAIOS_TASK_MMIO) under Renode.
#
# An unprivileged task granted GPIOA drives it directly (writes and reads back
# ODR), faults on RCC outside its window, and is restarted still holding the
# window; a task with no grant faults on GPIOA and is killed; bad grants (SRAM,
# misaligned) are refused. Then the v_upipe example again with MMIO on too:
# four per-task regions, the widest PendSV burst.
#
# Exit: 0 = pass (or skipped when the toolchain/renode is absent), 1 = fail.
set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build_sitl_mmio"

for bin in arm-none-eabi-gcc cmake renode; do
  command -v "$bin" >/dev/null 2>&1 || { echo "SKIP: '$bin' not installed"; exit 0; }
done

export srctree="$ROOT_DIR/extern/NavHAL"

# build_and_run <dir> <example> [cmake args...]: sets $clean to the UART.
build_and_run() {
  local dir="$1" ex="$2"; shift 2
  echo "=== building $ex (NAVHAL) $* ==="
  rm -rf "$dir"
  if ! cmake -S "$ROOT_DIR" -B "$dir" -DNAVHAL=ON -DEXAMPLES=ON -DVAIOS_CONSOLE_DMA=OFF \
         -DVAIOS_EXAMPLE="$ex" "$@" >/tmp/sitl_mmio_cfg.log 2>&1 \
     || ! cmake --build "$dir" -j >/tmp/sitl_mmio_bld.log 2>&1; then
    echo "FAIL: build (see /tmp/sitl_mmio_bld.log)"; tail -15 /tmp/sitl_mmio_bld.log
    exit 1
  fi
  local log; log="$(mktemp)"
  timeout 180s renode --console --disable-xwt \
      -e "\$bin=@$dir/examples/main" -e '$run="6.0"' \
      -e "include @$ROOT_DIR/tools/renode.resc" </dev/null >"$log" 2>&1 || true
  clean="$(sed -E 's/.*usart2: \[[^]]*\] ?//; s/\x1b\[[0-9;]*m//g' "$log")"
  rm -f "$log"
  echo "---- captured UART ----"
  echo "$clean" | grep -aE '\[(mm|up)\]|contained|PANIC' || true
  echo "-----------------------"
}

fail=0
check() { # $1 = description, $2 = 0/1 result
  if [ "$2" -eq 1 ]; then echo "  PASS: $1"; else echo "  FAIL: $1"; fail=1; fi
}
has() { echo "$clean" | grep -aqE "$1" && echo 1 || echo 0; }

build_and_run "$BUILD_DIR" MMIO_USER
check "granted task drove GPIOA, twice (restart kept the window)" \
  "$([ "$(echo "$clean" | grep -ac '\[mm\] gpio ok')" -eq 2 ] && echo 1 || echo 0)"
check "RCC, outside the window, faulted" "$(has 'contained \| addr 0x40023830')"
check "ungranted task faulted on GPIOA"  "$(has 'contained \| addr 0x40020014')"
check "3 faults, 1 restart"              "$(has '\[mm\] faults=3 restarts=1')"
check "no access got through, no bad grant accepted" "$(has 'BUG' | tr 01 10)"
check "no kernel panic"                  "$(has 'KERNEL PANIC' | tr 01 10)"

cfg="$(mktemp)"
cat "$ROOT_DIR/navhal.config" > "$cfg"
echo "CONFIG_VAIOS_TASK_MMIO=y" >> "$cfg"
build_and_run "${BUILD_DIR}_upipe" UPIPE -DVAIOS_MODULE_BUS=ON -DNAVHAL_CONFIG_FILE="$cfg"
rm -f "$cfg"
check "four regions a task: upipe data ok" "$(has '\[up\] data ok')"
check "four regions a task: intruder still faults" "$(has '\[up\] faults=1 last addr')"
check "four regions a task: no panic"      "$(has 'KERNEL PANIC' | tr 01 10)"

if [ "$fail" -eq 0 ]; then
  echo "=== SITL mmio_user: ALL PASS ==="
  exit 0
fi
echo "=== SITL mmio_user: FAIL ==="
exit 1
