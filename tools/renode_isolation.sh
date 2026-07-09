#!/usr/bin/env bash
# tools/renode_isolation.sh — Stage-5 isolation regression under Renode.
#
# Builds the mpu_user_demo image, runs it headless in Renode, and asserts the
# unprivileged-flip guarantees hold: the task runs unprivileged (nPRIV=1), does
# real work through syscalls, and a deliberate kernel access is trapped by the
# MPU with a clean fault. This exercises the ARM-only enforcement code (context
# switch, MPU regions, nPRIV, SVC dispatch, fault handlers) that host gcov cannot
# reach — the one structural coverage gap.
#
# Exit: 0 = pass (or skipped when the toolchain/renode is absent), 1 = fail.
set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT_DIR="$ROOT_DIR/tools"
BUILD_DIR="$ROOT_DIR/build_sitl_isolation"

for bin in arm-none-eabi-gcc cmake renode; do
  command -v "$bin" >/dev/null 2>&1 || { echo "SKIP: '$bin' not installed"; exit 0; }
done

export srctree="$ROOT_DIR/extern/NavHAL"

echo "=== building mpu_user_demo (NAVHAL) ==="
if ! cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DNAVHAL=ON -DEXAMPLES=ON \
       -DVAIOS_EXAMPLE=MPU_USER_DEMO >/tmp/sitl_iso_cfg.log 2>&1 \
   || ! cmake --build "$BUILD_DIR" -j >/tmp/sitl_iso_bld.log 2>&1; then
  echo "FAIL: build (see /tmp/sitl_iso_bld.log)"; tail -15 /tmp/sitl_iso_bld.log
  exit 1
fi

# Run headless; retry a cold-start hiccup (same hedge as run_all_tests.sh sitl).
log="$(mktemp)"
for attempt in 1 2 3; do
  : > "$log"
  timeout 120s renode --console --disable-xwt \
      -e "\$bin=@$BUILD_DIR/examples/main" \
      -e '$run="4.0"' \
      -e "include @$SCRIPT_DIR/renode.resc" </dev/null >"$log" 2>&1 || true
  grep -aq 'mpu_user_demo' "$log" && break
  echo "  renode produced no on-target output (attempt $attempt); retrying ..."
done

# Strip renode's "usart2: [ts]" prefix and ANSI so the asserts see clean UART.
clean="$(sed -E 's/.*usart2: \[[^]]*\] ?//; s/\x1b\[[0-9;]*m//g' "$log")"
echo "---- captured UART ----"
echo "$clean" | grep -aE 'user\]|nPRIV|malloc|MPU fault|PANIC|Halt' || true
echo "-----------------------"
rm -f "$log"

fail=0
assert() { # $1 = description, $2 = extended-regex the UART must contain
  if echo "$clean" | grep -aqE "$2"; then
    echo "  PASS: $1"
  else
    echo "  FAIL: $1  (missing pattern: $2)"; fail=1
  fi
}
assert "task runs unprivileged (nPRIV=1)"          'nPRIV=1'
assert "syscall path works (malloc via syscall)"    'malloc=.*via syscall'
assert "MPU traps the kernel-memory escape"         'MPU fault in task .* fault addr 0x20000010'

if [ "$fail" -eq 0 ]; then
  echo "=== SITL isolation: ALL PASS ==="
  exit 0
fi
echo "=== SITL isolation: FAIL ==="
exit 1
