#!/usr/bin/env bash
# =============================================================================
# tools/mem_stress_ab.sh — build + run the MEM_STRESS example against BOTH heap
# allocator backends (segregated-fit and TLSF) under QEMU, capture the per-op
# logs, and render an A/B comparison plot.
#
# QEMU (netduinoplus2, semihosting) is used rather than Renode: it is far
# faster, and with the SYS_ELAPSED-backed cycle counter (portable/cortex-m4)
# v_perf_cycles() yields real virtual-clock timing. The deterministic
# search-cost metric (free-list probes) is platform-independent regardless.
#
# For each backend it:
#   1. configures a non-NAVHAL build with -DVAIOS_HEAP_ALGO=<backend>
#   2. builds examples/main (VAIOS_EXAMPLE=MEM_STRESS)
#   3. runs it under QEMU, streaming semihosting output to a log, and stops as
#      soon as the @MSEND marker appears (the example then spins forever).
# Then it feeds both logs to tools/plot_mem_stress.py.
#
# Usage:
#   tools/mem_stress_ab.sh                 # both backends
#   ICOUNT=1 tools/mem_stress_ab.sh        # deterministic timing (slower)
#   tools/mem_stress_ab.sh SEGLIST         # a single backend
#
# Requires: arm-none-eabi-gcc, qemu-system-arm, python3 + matplotlib/numpy.
# Outputs:  build_ms_<backend>/, /tmp/mem_stress_<backend>.log,
#           mem_stress_probes.png, mem_stress_cycles.png (repo root).
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$ROOT_DIR"
export srctree="${srctree:-$ROOT_DIR/extern/NavHAL}"

# -icount makes QEMU's virtual clock deterministic (cycles ∝ instructions) but
# is much slower; off by default (host-time timing, still fine for probes).
QEMU_EXTRA=""
[ "${ICOUNT:-0}" != "0" ] && QEMU_EXTRA="-icount shift=0"
MAX_WAIT="${MAX_WAIT:-90}" # seconds to wait for @MSEND before giving up

BACKENDS=("$@")
[ "${#BACKENDS[@]}" -eq 0 ] && BACKENDS=(SEGLIST TLSF)

have() { command -v "$1" >/dev/null 2>&1; }
have arm-none-eabi-gcc || { echo "need arm-none-eabi-gcc" >&2; exit 2; }
have qemu-system-arm   || { echo "need qemu-system-arm" >&2; exit 2; }

run_qemu() { # $1=elf  $2=logfile
  ( timeout "$((MAX_WAIT + 10))" qemu-system-arm -M netduinoplus2 -cpu cortex-m4 \
      -kernel "$1" -nographic -semihosting $QEMU_EXTRA >"$2" 2>&1 ) &
  local qp=$! i
  for ((i = 0; i < MAX_WAIT; i++)); do
    grep -qa '@MSEND' "$2" && break
    kill -0 "$qp" 2>/dev/null || break # qemu died on its own
    sleep 1
  done
  kill "$qp" 2>/dev/null; wait "$qp" 2>/dev/null
}

declare -A LOGS
for bk in "${BACKENDS[@]}"; do
  bld="$ROOT_DIR/build_ms_$(echo "$bk" | tr '[:upper:]' '[:lower:]')"
  log="/tmp/mem_stress_$(echo "$bk" | tr '[:upper:]' '[:lower:]').log"
  echo "########## $bk ##########"

  echo "  configuring + building ($bld) ..."
  if ! cmake -S "$ROOT_DIR" -B "$bld" -DNAVHAL=OFF -DEXAMPLES=ON \
         -DVAIOS_EXAMPLE=MEM_STRESS -DVAIOS_HEAP_ALGO="$bk" \
         >"$bld.cfg.log" 2>&1 \
     || ! cmake --build "$bld" -j >"$bld.bld.log" 2>&1; then
    echo "  BUILD FAILED for $bk (see $bld.bld.log)"; tail -15 "$bld.bld.log"; exit 1
  fi

  echo "  running under QEMU (waiting for @MSEND, <=${MAX_WAIT}s) ..."
  : > "$log"
  run_qemu "$bld/examples/main" "$log"
  rows="$(grep -ac '@MS,' "$log" || true)"
  echo "  captured $rows operation rows -> $log"
  if [ "${rows:-0}" -eq 0 ]; then
    echo "  WARNING: no rows; last 12 lines:"; tail -12 "$log" | sed 's/^/    | /'
  fi
  LOGS[$bk]="$log"
done

# --------------------------------------------------------------- plot ---------
if ! python3 -c "import matplotlib, numpy" 2>/dev/null; then
  echo "python matplotlib/numpy missing — logs are in /tmp, skipping plot." >&2
  exit 0
fi
args=(); for bk in "${BACKENDS[@]}"; do args+=("$bk=${LOGS[$bk]}"); done
echo "########## plotting ##########"
python3 "$SCRIPT_DIR/plot_mem_stress.py" "${args[@]}" -o "$ROOT_DIR/mem_stress"
