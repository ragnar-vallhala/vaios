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
# Two cases:
#   * context switch — CTX_SWITCH_SMOKE no-HAL soft-float, run N times, every run
#     must print "[SMOKE] PASS" with no HardFault (the PSP=0 race).
#   * clock bring-up — CLOCK_BOOT_SMOKE NAVHAL soft-float with clock_setup=1 must
#     print "[CLOCK] PASS", i.e. v_init returns instead of spinning forever in
#     NavHAL's PLL-ready wait (bounded-clock fix; navhal-issues.md).
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
echo "context-switch clean PASS: $pass/$RUNS"
ctx_ok=$([ "$pass" -eq "$RUNS" ] && echo 1 || echo 0)

# --- clock bring-up case: NAVHAL build with internal_clock_setup=1 must not hang
# under QEMU (NavHAL's clock-ready waits are bounded and fall back to HSI). ---
echo
echo "=== build CLOCK_BOOT_SMOKE (NAVHAL, soft-float) ==="
export srctree="${srctree:-$ROOT_DIR/extern/NavHAL}"
CB="$ROOT_DIR/build_qemu_clock"
rm -rf "$CB"
clock_ok=0
if cmake -S . -B "$CB" -DNAVHAL=ON \
       -DNAVHAL_CONFIG_FILE="$ROOT_DIR/tests/configs/navhal_softfp.config" \
       -DEXAMPLES=ON -DVAIOS_EXAMPLE=CLOCK_BOOT_SMOKE >/tmp/qc_cfg.log 2>&1 \
   && cmake --build "$CB" --target main -j"$(nproc)" >/tmp/qc_bld.log 2>&1; then
  out=$(timeout 8 qemu-system-arm -M netduinoplus2 -cpu cortex-m4 \
          -kernel "$CB/examples/main" -nographic -semihosting 2>&1)
  if echo "$out" | grep -q '\[CLOCK\] PASS'; then
    echo "clock bring-up: PASS (v_init returned, no hang)"; clock_ok=1
  else
    echo "clock bring-up: FAIL (v_init hung / no PASS line)"
  fi
else
  echo "clock bring-up: FAIL (build error)"
  grep -iE 'error|does not support' /tmp/qc_cfg.log /tmp/qc_bld.log | head -5
fi

echo
if [ "$ctx_ok" = 1 ] && [ "$clock_ok" = 1 ]; then echo "=== qemu smoke: ALL PASS ==="; exit 0; fi
echo "=== qemu smoke: FAILED (ctx_ok=$ctx_ok clock_ok=$clock_ok) ==="; exit 1
