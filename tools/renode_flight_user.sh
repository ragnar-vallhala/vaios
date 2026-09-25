#!/usr/bin/env bash
# tools/renode_flight_user.sh — the release gate for unprivileged operation
# (roadmap M7). Builds examples/58_flight_user.c and runs it headless: a
# flight-shaped app in which EVERY application task is unprivileged — a 1 kHz
# sensor ISR publishing to the bus, an estimator blocking on it, a controller
# holding a 5 ms cadence with task_delay_until, and a logger draining a telemetry
# queue — with each task naming itself, reading its own perf counters, and
# spawning and then ending a worker of its own.
#
# Exit: 0 = pass (or skipped when the toolchain/renode is absent), 1 = fail.
set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT_DIR="$ROOT_DIR/tools"
BUILD_DIR="$ROOT_DIR/build_sitl_flight_user"

for bin in arm-none-eabi-gcc cmake renode; do
  command -v "$bin" >/dev/null 2>&1 || { echo "SKIP: '$bin' not installed"; exit 0; }
done

export srctree="$ROOT_DIR/extern/NavHAL"

echo "=== building flight_user (NAVHAL) ==="
mkdir -p "$BUILD_DIR"
if ! cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DNAVHAL=ON -DEXAMPLES=ON \
       -DVAIOS_EXAMPLE=FLIGHT_USER >"$BUILD_DIR/cfg.log" 2>&1 \
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
  grep -aq '\[flight\]' "$log" && break
  echo "  renode produced no on-target output (attempt $attempt); retrying ..."
done

clean="$(sed -E 's/.*usart2: \[[^]]*\] ?//; s/\x1b\[[0-9;]*m//g' "$log")"
echo "---- captured UART ----"
echo "$clean" | grep -aE '\[flight\]|flight_user|PANIC|Halt|fault' || true
echo "-----------------------"
rm -f "$log"

fail=0
assert_present() { if echo "$clean" | grep -aqE "$2"; then echo "  PASS: $1"
  else echo "  FAIL: $1  (missing: $2)"; fail=1; fi; }
assert_absent() { if echo "$clean" | grep -aqE "$2"; then echo "  FAIL: $1  (saw: $2)"; fail=1
  else echo "  PASS: $1"; fi; }
assert_present "estimator unprivileged"     '\[flight\] estimator id=[0-9]+ nPRIV=1'
assert_present "controller unprivileged"    '\[flight\] controller id=[0-9]+ nPRIV=1'
assert_present "logger unprivileged"        '\[flight\] logger id=[0-9]+ nPRIV=1'
assert_present "spawned workers unpriv"     '\[flight\] worker nPRIV=1'
assert_present "estimator kept up"          '\[flight\] estimator PASS'
assert_present "controller held cadence"    '\[flight\] controller PASS'
assert_present "logger drained telemetry"   '\[flight\] logger PASS'
assert_absent  "no task failed"             '\[flight\] [a-z]+ FAIL'
assert_absent  "no boundary complaints"     'bad self-info|no perf counters|spawn failed|kill failed|starved|torn=|cadence '
assert_absent  "no kernel panic / fault"   'KERNEL PANIC|System Halted|MPU fault'

if [ "$fail" -eq 0 ]; then
  echo "=== SITL unprivileged flight app: ALL PASS ==="
  exit 0
fi
echo "=== SITL unprivileged flight app: FAIL ==="
exit 1
