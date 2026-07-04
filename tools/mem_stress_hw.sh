#!/usr/bin/env bash
# =============================================================================
# tools/mem_stress_hw.sh — run the MEM_STRESS example against BOTH heap backends
# on a real Nucleo-F401RE and capture the per-op logs over the ST-Link VCP.
#
# On hardware v_perf_cycles() reads the genuine DWT CYCCNT, so unlike the QEMU
# A/B (tools/mem_stress_ab.sh) the `cycles` column here is real cycle-accurate
# timing — the measurement the blog post defers to the board. The deterministic
# `probes` column is identical to the emulated run.
#
# For each backend it: builds NAVHAL firmware with -DVAIOS_HEAP_ALGO=<bk>,
# objcopies to .bin, flashes via st-flash, then captures UART for a window
# while the board reboots into the run.
#
# Usage:
#   tools/mem_stress_hw.sh                 # both backends
#   CAPTURE_SECS=40 tools/mem_stress_hw.sh
#   PORT=/dev/ttyACM1 tools/mem_stress_hw.sh SEGLIST
#
# Outputs: build_hw_<bk>/, and <BK>_hw.txt in the repo root (raw capture).
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
cd "$ROOT"
export srctree="${srctree:-$ROOT/extern/NavHAL}"

PORT="${PORT:-/dev/ttyACM0}"
CAPTURE_SECS="${CAPTURE_SECS:-35}"
BACKENDS=("$@")
[ "${#BACKENDS[@]}" -eq 0 ] && BACKENDS=(SEGLIST TLSF)

for t in arm-none-eabi-gcc arm-none-eabi-objcopy cmake st-flash st-info; do
  command -v "$t" >/dev/null || { echo "missing tool: $t" >&2; exit 2; }
done
st-info --probe 2>&1 | grep -qi 'stlink\|serial' || { echo "no ST-Link board" >&2; exit 2; }
[ -e "$PORT" ] || { echo "serial port $PORT absent" >&2; exit 2; }

for bk in "${BACKENDS[@]}"; do
  bld="$ROOT/build_hw_$(echo "$bk" | tr '[:upper:]' '[:lower:]')"
  out="$ROOT/${bk}_hw.txt"
  echo "########## $bk (hardware) ##########"

  echo "  building NAVHAL firmware ($bld) ..."
  if ! cmake -S "$ROOT" -B "$bld" -DNAVHAL=ON -DEXAMPLES=ON \
         -DVAIOS_EXAMPLE=MEM_STRESS -DVAIOS_HEAP_ALGO="$bk" \
         >"$bld.cfg.log" 2>&1 \
     || ! cmake --build "$bld" -j >"$bld.bld.log" 2>&1; then
    echo "  BUILD FAILED (see $bld.bld.log)"; tail -15 "$bld.bld.log"; exit 1
  fi
  arm-none-eabi-objcopy -O binary "$bld/examples/main" "$bld/examples/main.bin"

  echo "  flashing ..."
  flashed=0
  for attempt in 1 2 3; do
    if st-flash --connect-under-reset write "$bld/examples/main.bin" 0x8000000 \
         >"/tmp/hw_${bk}_flash.log" 2>&1; then flashed=1; break; fi
    echo "    flash attempt $attempt failed; retrying ..."; sleep 1
  done
  if [ "$flashed" -ne 1 ]; then
    echo "  FLASH FAILED (see /tmp/hw_${bk}_flash.log):"; tail -5 "/tmp/hw_${bk}_flash.log"
    echo "  Recovery: hold the black RESET, re-run, release when writing starts."
    exit 1
  fi

  echo "  capturing UART on $PORT for ${CAPTURE_SECS}s (board reboots into the run) ..."
  stty -F "$PORT" 115200 raw -echo
  : > "$out"
  ( timeout "$CAPTURE_SECS" cat "$PORT" > "$out" 2>/dev/null ) &
  cap=$!
  sleep 1
  st-flash reset >/dev/null 2>&1 || true
  wait "$cap" || true

  rows="$(grep -ac '@MS,' "$out" || true)"
  start="$(grep -a '@MSTART' "$out" | sed -E 's/.*@MSTART/@MSTART/' | head -1)"
  echo "  captured $rows rows -> $out   ${start:+[$start]}"
  if [ "${rows:-0}" -eq 0 ]; then
    echo "  WARNING: no rows; last 10 lines:"; tail -10 "$out" | sed 's/^/    | /'
  fi
done

echo "done. raw captures: ${BACKENDS[*]/%/_hw.txt}"
