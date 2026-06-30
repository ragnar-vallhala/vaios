#!/usr/bin/env bash
# =============================================================================
# tools/test_build_matrix.sh — vaios must build coherently across FPU modes.
#
# Regression guard for the FPU-coherence bug (problems/vaios-issues.md, Issue 2):
# vaios's float ABI is mirrored from NavHAL's CONFIG_USE_FPU, so a soft-float
# NavHAL config must yield a soft-float vaios that assembles. Before the fix, a
# soft-float build left _FPU_ENABLED defined (from NavHAL's navhal_target.h) and
# port.c's `vstmdb/vldmia {s16-s31}` failed to assemble.
#
# Builds the `vaios` static lib (cross, arm-none-eabi) in each mode:
#   fpu-hard   default navhal.config (CONFIG_USE_FPU=y)        -> -mfloat-abi=hard
#   fpu-soft   tests/configs/navhal_softfp.config (FPU off)    -> -mfloat-abi=soft
#
# Exit code: number of build modes that FAILED (0 = all built).
# =============================================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
cd "$ROOT_DIR"
# NavHAL's Kconfig resolves source globs relative to $srctree.
export srctree="${srctree:-$ROOT_DIR/extern/NavHAL}"

if ! command -v arm-none-eabi-gcc >/dev/null 2>&1; then
  echo "SKIP: arm-none-eabi-gcc not found"; exit 0
fi

fail=0
build_one() {
  local name="$1" expect_abi="$2"; shift 2
  local bdir="$ROOT_DIR/build_matrix/$name"
  rm -rf "$bdir"
  printf '%-10s ' "$name"
  if cmake -S . -B "$bdir" "$@" >/tmp/bm_cfg.log 2>&1 \
     && cmake --build "$bdir" --target vaios -j"$(nproc)" >/tmp/bm_bld.log 2>&1; then
    local abi
    abi=$(grep -hoE '\-mfloat-abi=[a-z]+' "$bdir"/portable/CMakeFiles/portable.dir/flags.make 2>/dev/null | head -1)
    if [ "$abi" = "-mfloat-abi=$expect_abi" ]; then
      echo "PASS  ($abi)"
    else
      echo "FAIL  (expected -mfloat-abi=$expect_abi, got '$abi')"; fail=$((fail+1))
    fi
  else
    echo "FAIL  (build error)"
    grep -iE 'error|does not support' /tmp/bm_cfg.log /tmp/bm_bld.log | head -5
    fail=$((fail+1))
  fi
}

echo "=== vaios FPU build matrix ==="
build_one fpu-hard hard -DNAVHAL=ON
build_one fpu-soft soft -DNAVHAL=ON \
  -DNAVHAL_CONFIG_FILE="$ROOT_DIR/tests/configs/navhal_softfp.config"

if [ "$fail" -eq 0 ]; then echo "=== build matrix: ALL PASS ==="; else echo "=== build matrix: $fail FAILED ==="; fi
exit "$fail"
