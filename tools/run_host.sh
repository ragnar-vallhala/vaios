#!/usr/bin/env bash
# tools/run_host.sh — build and run a vaios example natively on the host, on the
# portable/host/ port (ucontext + SIGALRM preemptive scheduler). No ARM toolchain
# needed. See docs/plan/HOST_PORT_PLAN.md.
#
# Usage: tools/run_host.sh [VAIOS_EXAMPLE]   (default: HOST_DEMO)
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
EXAMPLE="${1:-HOST_DEMO}"
BUILD_DIR="$ROOT_DIR/build_host"

command -v cmake >/dev/null || { echo "cmake not found" >&2; exit 1; }

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
      -DVAIOS_PORT=host -DNAVHAL=OFF \
      -DEXAMPLES=ON -DVAIOS_EXAMPLE="$EXAMPLE" >/dev/null
cmake --build "$BUILD_DIR" --target main --parallel >/dev/null

echo "=== running $EXAMPLE on the host port ==="
exec "$BUILD_DIR/examples/main"
