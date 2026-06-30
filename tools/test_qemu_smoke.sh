#!/usr/bin/env bash
# =============================================================================
# tools/test_qemu_smoke.sh — vaios must boot and context-switch under QEMU,
# deterministically, every run.
#
# Regression guard for the first-context-switch race (problems/vaios-issues.md
# Issue 1): scheduler_start used to launch the first task with interrupts live,
# so a SysTick could pend PendSV before the svc established PSP — PendSV then
# saved to PSP=0 and bus-faulted at 0xFFFFFFDC. Before the fix this failed ~7/8
# runs; after, 0/N.
#
# Builds the CTX_SWITCH_SMOKE example no-HAL + soft-float (so it runs on QEMU's
# FPU-less cortex-m4), runs it N times, and requires every run to print
# "[SMOKE] PASS" with no HardFault.
#
# Env: QEMU_SMOKE_RUNS (default 16). Exit: 0 all clean, 1 otherwise.
# =============================================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$ROOT_DIR"

if ! command -v qemu-system-arm >/dev/null 2>&1; then echo "SKIP: qemu-system-arm not found"; exit 0; fi
if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then echo "SKIP: arm-none-eabi-gcc not found"; exit 0; fi

RUNS="${QEMU_SMOKE_RUNS:-16}"
B="$ROOT_DIR/build_qemu_smoke"
ELF="$B/examples/main"

echo "=== build CTX_SWITCH_SMOKE (no-HAL, soft-float) ==="
rm -rf "$B"
if ! cmake -S . -B "$B" -DNAVHAL=OFF -DVAIOS_FPU=OFF -DEXAMPLES=ON \
        -DVAIOS_EXAMPLE=CTX_SWITCH_SMOKE >/tmp/qs_cfg.log 2>&1 \
   || ! cmake --build "$B" --target main -j"$(nproc)" >/tmp/qs_bld.log 2>&1; then
  echo "FAIL: build error"; grep -iE 'error|does not support' /tmp/qs_cfg.log /tmp/qs_bld.log | head; exit 1
fi

echo "=== run x$RUNS in QEMU (netduinoplus2) ==="
pass=0
for i in $(seq 1 "$RUNS"); do
  out=$(timeout 5 qemu-system-arm -M netduinoplus2 -cpu cortex-m4 \
          -kernel "$ELF" -nographic -semihosting -d guest_errors 2>&1)
  if echo "$out" | grep -qiE '0xFFFFFFDC|HARDFAULT'; then
    printf 'F'   # bus fault — the race
  elif echo "$out" | grep -q '\[SMOKE\] PASS'; then
    printf '.'; pass=$((pass+1))
  else
    printf '?'   # no fault but no PASS (hang / B not scheduled)
  fi
done
echo
echo "clean PASS: $pass/$RUNS"
[ "$pass" -eq "$RUNS" ] && { echo "=== qemu smoke: ALL PASS ==="; exit 0; }
echo "=== qemu smoke: FAILED ==="; exit 1
