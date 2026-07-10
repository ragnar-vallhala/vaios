#!/usr/bin/env bash
# =============================================================================
# tools/run_all_tests.sh — run every vaios test layer in one go.
#
# Layers (each skips cleanly if its prerequisites are missing):
#   host      Host-native unit suite           tools/run_tests.sh        (gcc)
#   build     Cross build matrix (FPU modes)   tools/test_build_matrix.sh (arm-gcc)
#   qemu      Context-switch smoke in QEMU     tools/test_qemu_smoke.sh  (arm-gcc, qemu)
#   coverage  Host gcov line/branch report     tools/coverage.sh         (gcov)
#   sitl      On-target unit tests in Renode   build + Renode            (arm-gcc, renode)
#   pitl      Hardware regression on a board   tools/run_hw_tests.sh     (st-flash + board)
#
# Usage:
#   tools/run_all_tests.sh                 # every layer whose tools are present
#   tools/run_all_tests.sh host sitl       # only the named layers
#   PORT=/dev/ttyACM1 tools/run_all_tests.sh
#
# Exit code: number of layers that FAILED (0 = all green; skipped layers do
# not count as failures).
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
PORT="${PORT:-/dev/ttyACM0}"
CAPTURE_SECS="${CAPTURE_SECS:-10}"
cd "$ROOT_DIR"

# NavHAL's Kconfig resolves paths relative to $srctree; any -DNAVHAL=ON build
# (sitl, pitl) fails to configure without it. Export once for every layer.
export srctree="${srctree:-$ROOT_DIR/extern/NavHAL}"

c_grn() { printf '\033[1;32m%s\033[0m' "$1"; }
c_red() { printf '\033[1;31m%s\033[0m' "$1"; }
c_yel() { printf '\033[1;33m%s\033[0m' "$1"; }
c_blu() { printf '\033[1;34m%s\033[0m' "$1"; }
hr() { printf '%s\n' "------------------------------------------------------------"; }
header() { echo; c_blu "########## $1 ##########"; echo; }

# Which layers to run.
ALL_LAYERS=(host build qemu coverage sitl pitl)
if [ "$#" -gt 0 ]; then
  LAYERS=("$@")
else
  LAYERS=("${ALL_LAYERS[@]}")
fi

declare -A STATUS   # layer -> PASS|FAIL|SKIP
declare -A DETAIL   # layer -> short note

have() { command -v "$1" >/dev/null 2>&1; }

# ---------------------------------------------------------------- host --------
run_host() {
  header "HOST UNIT TESTS"
  if ! have gcc; then STATUS[host]=SKIP; DETAIL[host]="no gcc"; return; fi
  if bash "$SCRIPT_DIR/run_tests.sh"; then
    STATUS[host]=PASS
  else
    STATUS[host]=FAIL; DETAIL[host]="see output above"
  fi
}

# --------------------------------------------------------------- build --------
run_build() {
  header "BUILD MATRIX (FPU coherence)"
  if ! have arm-none-eabi-gcc; then STATUS[build]=SKIP; DETAIL[build]="no arm toolchain"; return; fi
  if bash "$SCRIPT_DIR/test_build_matrix.sh"; then
    STATUS[build]=PASS
  else
    STATUS[build]=FAIL; DETAIL[build]="a build mode failed (see output above)"
  fi
}

# ---------------------------------------------------------------- qemu --------
run_qemu() {
  header "QEMU CONTEXT-SWITCH SMOKE"
  if ! have arm-none-eabi-gcc; then STATUS[qemu]=SKIP; DETAIL[qemu]="no arm toolchain"; return; fi
  if ! have qemu-system-arm; then STATUS[qemu]=SKIP; DETAIL[qemu]="qemu-system-arm not installed"; return; fi
  if bash "$SCRIPT_DIR/test_qemu_smoke.sh"; then
    STATUS[qemu]=PASS
  else
    STATUS[qemu]=FAIL; DETAIL[qemu]="context-switch smoke faulted (see output above)"
  fi
}

# ------------------------------------------------------------ coverage --------
run_coverage() {
  header "HOST COVERAGE (gcov)"
  if ! have gcc || ! have gcov; then
    STATUS[coverage]=SKIP; DETAIL[coverage]="no gcc/gcov"; return
  fi
  local log; log="$(mktemp)"
  if bash "$SCRIPT_DIR/coverage.sh" | tee "$log"; then
    STATUS[coverage]=PASS
    DETAIL[coverage]="$(grep -E '^TOTAL' "$log" | sed -E 's/ +/ /g')"
  else
    STATUS[coverage]=FAIL
  fi
  rm -f "$log"
}

# ---------------------------------------------------------------- sitl --------
# Build the on-target UNIT_TESTS runner and run it in Renode; pass iff the
# UART log reports ON-TARGET RESULT: ALL PASS.
run_sitl() {
  header "SITL — on-target unit tests in Renode"
  if ! have arm-none-eabi-gcc; then STATUS[sitl]=SKIP; DETAIL[sitl]="no arm toolchain"; return; fi
  if ! have renode; then STATUS[sitl]=SKIP; DETAIL[sitl]="renode not installed"; return; fi

  echo "building build_pil (NAVHAL, UNIT_TESTS) ..."
  if ! cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build_pil" \
         -DNAVHAL=ON -DEXAMPLES=ON -DVAIOS_EXAMPLE=UNIT_TESTS >/tmp/all_sitl_cfg.log 2>&1 \
     || ! cmake --build "$ROOT_DIR/build_pil" >/tmp/all_sitl_bld.log 2>&1; then
    # Surface the actual error (configure or compile) so a CI failure is
    # self-diagnosing instead of an opaque "build failed".
    echo "  ---- last 25 lines of build output ----"
    tail -n 25 /tmp/all_sitl_cfg.log /tmp/all_sitl_bld.log 2>/dev/null | sed 's/^/  | /'
    echo "  ---------------------------------------"
    STATUS[sitl]=FAIL; DETAIL[sitl]="build failed (see logs above)"; return
  fi

  # Detach stdin (</dev/null) so `--console` renode never tries to read from an
  # interactive terminal and can't block waiting on one. Defensive: renode's
  # stdout is already redirected to a file below, but a detached stdin keeps the
  # headless path deterministic. A retry hedges a genuine cold-start hiccup.
  local log; log="$(mktemp)"
  local attempt produced=0
  for attempt in 1 2; do
    : > "$log"
    timeout 120s renode --console --disable-xwt \
        -e "\$bin=@$ROOT_DIR/build_pil/examples/main" \
        -e '$run="4.0"' \
        -e "include @$SCRIPT_DIR/renode.resc" </dev/null >"$log" 2>&1 || true
    if grep -aq 'ON-TARGET' "$log"; then produced=1; break; fi
    echo "  renode produced no on-target output (attempt $attempt); retrying ..."
  done
  sed -E 's/.*usart2: \[[^]]*\] ?//; s/\x1b\[[0-9;]*m//g' "$log" \
    | grep -aE 'Suite:|Results:|ON-TARGET' || true
  if grep -aq 'ON-TARGET RESULT: ALL PASS' "$log"; then
    STATUS[sitl]=PASS
  else
    cp "$log" /tmp/all_sitl_uart.log
    # Surface what renode actually said so an empty/failed capture is
    # self-diagnosing instead of an opaque "no UART". An empty capture is
    # usually renode erroring before the machine ran (bad ELF, display, a
    # monitor prompt that hung until the timeout) — that reason is in here.
    echo "  ---- last 15 lines of renode output (/tmp/all_sitl_uart.log) ----"
    sed -E 's/\x1b\[[0-9;]*m//g' "$log" | tail -n 15 | sed 's/^/  | /'
    echo "  ----------------------------------------------------------------"
    if [ "$produced" -eq 0 ]; then
      STATUS[sitl]=FAIL; DETAIL[sitl]="renode captured no UART after 2 tries (see tail above / /tmp/all_sitl_uart.log)"
    else
      STATUS[sitl]=FAIL; DETAIL[sitl]="ran but no ALL PASS (see tail above / /tmp/all_sitl_uart.log)"
    fi
  fi
  rm -f "$log"
}

# ---------------------------------------------------------------- pitl --------
run_pitl() {
  header "PITL — hardware regression on a connected board"
  if ! have arm-none-eabi-gcc || ! have st-flash || ! have st-info; then
    STATUS[pitl]=SKIP; DETAIL[pitl]="no arm toolchain / st-tools"; return
  fi
  # Cheap, side-effect-free gate first: no serial port means no board to talk
  # to, so skip before poking the ST-Link with `st-info --probe`.
  if [ ! -e "$PORT" ]; then
    STATUS[pitl]=SKIP; DETAIL[pitl]="serial port $PORT absent (set PORT=...)"; return
  fi
  if ! st-info --probe 2>/dev/null | grep -qi 'stlink\|serial'; then
    STATUS[pitl]=SKIP; DETAIL[pitl]="no ST-Link board detected"; return
  fi
  if CAPTURE_SECS="$CAPTURE_SECS" PORT="$PORT" bash "$SCRIPT_DIR/run_hw_tests.sh"; then
    STATUS[pitl]=PASS
  else
    STATUS[pitl]=FAIL; DETAIL[pitl]="$? example(s) failed"
  fi
}

# ----------------------------------------------------------- dispatch ---------
for layer in "${LAYERS[@]}"; do
  case "$layer" in
    host)     run_host ;;
    build)    run_build ;;
    qemu)     run_qemu ;;
    coverage) run_coverage ;;
    sitl)     run_sitl ;;
    pitl)     run_pitl ;;
    *) echo "unknown layer: $layer (valid: ${ALL_LAYERS[*]})" >&2; exit 2 ;;
  esac
done

# ------------------------------------------------------------- summary --------
echo; hr; c_blu "                 ALL-TESTS ROLL-UP"; echo; hr
fails=0
for layer in "${LAYERS[@]}"; do
  st="${STATUS[$layer]:-SKIP}"
  case "$st" in
    PASS) tag="$(c_grn PASS)" ;;
    FAIL) tag="$(c_red FAIL)"; fails=$((fails+1)) ;;
    *)    tag="$(c_yel SKIP)" ;;
  esac
  printf '  %-9s %s  %s\n' "$layer" "$tag" "${DETAIL[$layer]:-}"
done
hr
if [ "$fails" -eq 0 ]; then c_grn "=== ALL RUN LAYERS PASSED ==="; echo
else c_red "=== $fails LAYER(S) FAILED ==="; echo; fi
exit "$fails"
