#!/usr/bin/env bash
# tools/renode_bus_user.sh — unprivileged Bus IPC access under Renode (plan B9).
#
# Builds examples/57_bus_user.c (MPU user separation + DEVFS + SVC) and runs it
# headless: three UNPRIVILEGED tasks pass 400 messages through v_bus_open /
# v_bus_send / v_bus_recv — one producer, two consumers reading in order with no
# gaps, the pipe topic admitting exactly one reader — and each checks that
# kernel-memory payload/rx pointers come back as EFAULT instead of faulting.
#
# Exit: 0 = pass (or skipped when the toolchain/renode is absent), 1 = fail.
set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT_DIR="$ROOT_DIR/tools"
BUILD_DIR="$ROOT_DIR/build_sitl_bus_user"

for bin in arm-none-eabi-gcc cmake renode; do
  command -v "$bin" >/dev/null 2>&1 || { echo "SKIP: '$bin' not installed"; exit 0; }
done

export srctree="$ROOT_DIR/extern/NavHAL"

echo "=== building bus_user (NAVHAL) ==="
mkdir -p "$BUILD_DIR"
if ! cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DNAVHAL=ON -DEXAMPLES=ON \
       -DVAIOS_EXAMPLE=BUS_USER >"$BUILD_DIR/cfg.log" 2>&1 \
   || ! cmake --build "$BUILD_DIR" -j >"$BUILD_DIR/bld.log" 2>&1; then
  echo "FAIL: build (see $BUILD_DIR/bld.log)"; tail -15 "$BUILD_DIR/bld.log"
  exit 1
fi

log="$(mktemp)"
for attempt in 1 2 3; do
  : > "$log"
  timeout 120s renode --console --disable-xwt \
      -e "\$bin=@$BUILD_DIR/examples/main" \
      -e '$run="8.0"' \
      -e "include @$SCRIPT_DIR/renode.resc" </dev/null >"$log" 2>&1 || true
  grep -aq '\[busu\]' "$log" && break
  echo "  renode produced no on-target output (attempt $attempt); retrying ..."
done

clean="$(sed -E 's/.*usart2: \[[^]]*\] ?//; s/\x1b\[[0-9;]*m//g' "$log")"
echo "---- captured UART ----"
echo "$clean" | grep -aE '\[busu\]|bus_user|PANIC|Halt|fault' || true
echo "-----------------------"
rm -f "$log"

fail=0
assert_present() { if echo "$clean" | grep -aqE "$2"; then echo "  PASS: $1"
  else echo "  FAIL: $1  (missing: $2)"; fail=1; fi; }
assert_absent() { if echo "$clean" | grep -aqE "$2"; then echo "  FAIL: $1  (saw: $2)"; fail=1
  else echo "  PASS: $1"; fi; }
assert_present "producer runs unprivileged"  '\[busu\] P nPRIV=1'
assert_present "consumer A unprivileged"    '\[busu\] A nPRIV=1'
assert_present "consumer B unprivileged"    '\[busu\] B nPRIV=1'
assert_present "spawned task unprivileged"  '\[busu\] H nPRIV=1'
assert_present "spawned task ran"           '\[busu\] H PASS'
assert_present "tasks know themselves"       '\[busu\] id=[0-9]+ prio=[0-9]+'
assert_present "producer sent + EFAULTs"    '\[busu\] P PASS'
assert_present "consumer A read in order"   '\[busu\] A PASS'
assert_present "consumer B read in order"   '\[busu\] B PASS'
assert_absent  "no task failed"             '\[busu\] [PABH] FAIL'
assert_absent  "self-info intact"           '\[busu\] self:'
assert_absent  "ownership enforced"          'killed a stranger|kill own child failed|spawn escalated'
assert_absent  "queue fds work"              'queue send failed|queue got'
assert_absent  "no kernel panic / fault"   'KERNEL PANIC|System Halted|MPU fault'

if [ "$fail" -eq 0 ]; then
  echo "=== SITL bus IPC user access: ALL PASS ==="
  exit 0
fi
echo "=== SITL bus IPC user access: FAIL ==="
exit 1
