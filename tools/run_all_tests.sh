#!/usr/bin/env bash
# =============================================================================
# tools/run_all_tests.sh — run every vaios test layer in one go.
#
# Layers (each skips cleanly if its prerequisites are missing):
#   host      Host-native unit suite           tools/run_tests.sh        (gcc)
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

c_grn() { printf '\033[1;32m%s\033[0m' "$1"; }
c_red() { printf '\033[1;31m%s\033[0m' "$1"; }
c_yel() { printf '\033[1;33m%s\033[0m' "$1"; }
c_blu() { printf '\033[1;34m%s\033[0m' "$1"; }
hr() { printf '%s\n' "------------------------------------------------------------"; }
header() { echo; c_blu "########## $1 ##########"; echo; }

# Which layers to run.
ALL_LAYERS=(host coverage sitl pitl)
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
    STATUS[sitl]=FAIL; DETAIL[sitl]="build failed (see /tmp/all_sitl_bld.log)"; return
  fi

  local log; log="$(mktemp)"
  timeout 120s renode --console --disable-xwt \
      -e "\$bin=@$ROOT_DIR/build_pil/examples/main" \
      -e '$run="4.0"' \
      -e "include @$SCRIPT_DIR/renode.resc" >"$log" 2>&1 || true
  sed -E 's/.*usart2: \[[^]]*\] ?//; s/\x1b\[[0-9;]*m//g' "$log" \
    | grep -aE 'Suite:|Results:|ON-TARGET' || true
  if grep -aq 'ON-TARGET RESULT: ALL PASS' "$log"; then
    STATUS[sitl]=PASS
  else
    STATUS[sitl]=FAIL; DETAIL[sitl]="no ALL PASS in Renode UART (log /tmp/all_sitl_uart.log)"
    cp "$log" /tmp/all_sitl_uart.log
  fi
  rm -f "$log"
}

# ---------------------------------------------------------------- pitl --------
run_pitl() {
  header "PITL — hardware regression on a connected board"
  if ! have arm-none-eabi-gcc || ! have st-flash || ! have st-info; then
    STATUS[pitl]=SKIP; DETAIL[pitl]="no arm toolchain / st-tools"; return
  fi
  if ! st-info --probe 2>/dev/null | grep -qi 'stlink\|serial'; then
    STATUS[pitl]=SKIP; DETAIL[pitl]="no ST-Link board detected"; return
  fi
  if [ ! -e "$PORT" ]; then
    STATUS[pitl]=SKIP; DETAIL[pitl]="serial port $PORT absent (set PORT=...)"; return
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
