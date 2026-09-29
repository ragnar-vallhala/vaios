#!/usr/bin/env bash
# =============================================================================
# tools/pitl_run.sh — flash one already-built firmware and capture its UART.
#
# run_hw_tests.sh owns the curated regression list; this is the same
# flash-and-capture sequence for a single build directory, for the runs that are
# hardware-only (an SD card, real cycle counts) and are read rather than
# pass/fail-matched.
#
#   tools/pitl_run.sh <build-dir> [capture-secs]
#   tools/pitl_run.sh --kmsg <build-dir> [run-secs]   read the log over SWD
#
# Environment:
#   USB_LOC=<bus>-<port>     which probe; default 3-2. See tools/lib/probe.sh —
#                            the probes here share a serial, so the USB port is
#                            the only way to name one.
#   PORT=<tty>               where the board's UART lands; default: the single
#                            /dev/serial/by-id entry, else /dev/ttyACM0
#
# --kmsg needs no serial port at all: build with VAIOS_CONSOLE_TO_KMSG=y, let the
# firmware run, then read the kernel log ring out of RAM over SWD. That is the
# mode for a board whose ST-Link clone has no VCP. It does not disturb timing —
# unlike semihosting, which halts the core on every write and would wreck any
# cadence measurement.
#
# Exit: 0 if it flashed and captured anything, 1 otherwise. What the output
# MEANS is the caller's business — this script only gets it off the board.
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
# shellcheck source=lib/probe.sh
. "$SCRIPT_DIR/lib/probe.sh"

KMSG=0
if [ "${1:-}" = "--kmsg" ]; then KMSG=1; shift; fi

BUILD_DIR="${1:-}"
CAPTURE_SECS="${2:-15}"
[ -n "$BUILD_DIR" ] || { echo "usage: $0 <build-dir> [capture-secs]" >&2; exit 2; }

ELF="$BUILD_DIR/examples/main"
[ -f "$ELF" ] || ELF="$BUILD_DIR/examples/benchmark/benchmark"
[ -f "$ELF" ] || { echo "no firmware in $BUILD_DIR (examples/main or examples/benchmark/benchmark)" >&2; exit 2; }

probe_require_tools || exit 2
probe_present || {
  echo "no ST-Link at USB location $USB_LOC." >&2
  echo "Set USB_LOC=<bus>-<port>; the 0483 entries under /sys/bus/usb/devices/ list them." >&2
  exit 2
}

echo "=== flashing $(basename "$BUILD_DIR") -> probe at $USB_LOC, reading ${PORT:-SWD log ring} ==="

# The previous firmware may have parked the MCU in WFI or reconfigured the debug
# pins, which fails the first SWD handshake. A second attempt after a pause
# usually lands; past that it needs a finger on the reset button.
flashed=0
for attempt in 1 2 3; do
  if probe_flash "$ELF" /tmp/pitl_flash.log; then flashed=1; break; fi
  sleep 1
done
if [ "$flashed" -ne 1 ]; then
  echo "FLASH FAILED — last lines:" >&2
  tail -8 /tmp/pitl_flash.log >&2
  echo "Recovery: hold the board's reset button, re-run, release when writing starts." >&2
  exit 1
fi

if [ "$KMSG" -eq 1 ]; then
  command -v gdb-multiarch >/dev/null || { echo "--kmsg needs gdb-multiarch" >&2; exit 2; }
  echo "running for ${CAPTURE_SECS}s, then reading the log ring over SWD ..."
  sleep "$CAPTURE_SECS"

  oocd=$(probe_gdbserver /tmp/pitl_openocd.log)
  sleep 2
  # Halt, then read the ring symbolically: head/tail are absolute counters, so
  # the live text is [tail, head) modulo the ring size.
  gdb-multiarch -batch -nx \
    -ex "set pagination off" \
    -ex "set confirm off" \
    -ex "target extended-remote :$GDB_PORT" \
    -ex "interrupt" \
    -ex "printf \"KMSG_LIVE %u\\n\", systick_count" \
    -ex "printf \"KMSG_HEAD %u\\n\", kmsg_head" \
    -ex "printf \"KMSG_TAIL %u\\n\", kmsg_tail" \
    -ex "printf \"KMSG_SIZE %u\\n\", (unsigned)sizeof(kmsg_ring)" \
    -ex "dump binary value /tmp/pitl_kmsg.bin kmsg_ring" \
    "$ELF" >/tmp/pitl_gdb.log 2>&1
  kill "$oocd" 2>/dev/null; wait "$oocd" 2>/dev/null
  head=$(awk '/^KMSG_HEAD/{print $2}' /tmp/pitl_gdb.log)
  tail_=$(awk '/^KMSG_TAIL/{print $2}' /tmp/pitl_gdb.log)
  size=$(awk '/^KMSG_SIZE/{print $2}' /tmp/pitl_gdb.log)
  # gdb with no connection silently reads the ELF's .bss (all zeros) and would
  # report an empty ring as if that were the truth. A live target has ticked.
  live=$(awk '/^KMSG_LIVE/{print $2}' /tmp/pitl_gdb.log)
  if grep -q "could not connect\|Connection timed out" /tmp/pitl_gdb.log ||
     [ "${live:-0}" = "0" ]; then
    echo "gdb did not reach a RUNNING target (systick=${live:-unset})." >&2
    echo "Build with VAIOS_DEBUG_IN_SLEEP=y: once the idle task sleeps, the" >&2
    echo "debug AP loses bus access and reads come back as zeros." >&2
    tail -8 /tmp/pitl_gdb.log >&2
    exit 1
  fi
  if [ -z "${head:-}" ] || [ ! -s /tmp/pitl_kmsg.bin ]; then
    echo "could not read the log ring — gdb output:" >&2
    tail -15 /tmp/pitl_gdb.log >&2
    exit 1
  fi
  echo "---- kernel log ring (head=$head tail=$tail_ size=$size) ----"
  python3 - "$head" "$tail_" "$size" <<'PYEOF'
import sys
head, tail, size = (int(x) for x in sys.argv[1:4])
data = open('/tmp/pitl_kmsg.bin','rb').read()
# head/tail are absolute byte counters; the live window is [tail, head).
out = bytearray()
for i in range(tail, head):
    out.append(data[i % size])
sys.stdout.write(out.decode('ascii', 'replace'))
PYEOF
  echo
  echo "------------------------------------------------------------"
  exit 0
fi

# Resolve the port only NOW: with a USB-CDC console the port is created by the
# firmware we just flashed, so it cannot be checked beforehand. Wait for the host
# to enumerate it.
if [ -z "${PORT:-}" ]; then
  for i in $(seq 1 20); do
    mapfile -t ports < <(ls /dev/serial/by-id/* 2>/dev/null)
    if [ "${#ports[@]}" -ge 1 ]; then
      PORT="$(readlink -f "${ports[0]}")"
      break
    fi
    sleep 0.5
  done
  PORT="${PORT:-/dev/ttyACM0}"
fi
if [ ! -e "$PORT" ]; then
  echo "no serial port appeared (looked for /dev/serial/by-id/*, then $PORT)." >&2
  echo "A USART2 console needs a VCP or a USB-TTL adapter; a USB-CDC console" >&2
  echo "needs CONFIG_CONSOLE_ROUTE_CDC=y and DRV_USB_CDC=y. Or use --kmsg." >&2
  exit 1
fi
echo "    reading $PORT"

# Start capturing BEFORE the reset: a CDC port sends nothing until the host has
# opened it, so the banner only lands in the window if we reset after opening.
stty -F "$PORT" 115200 raw -echo
log="$(mktemp)"
( timeout "$CAPTURE_SECS" cat "$PORT" > "$log" 2>/dev/null ) &
cap=$!
sleep 1
probe_reset || true
wait "$cap" || true

echo "---- captured UART ($CAPTURE_SECS s) ----"
sed -E 's/\x1b\[[0-9;]*m//g' "$log" | tr -d '\r'
echo "----------------------------------------"
bytes=$(wc -c < "$log")
rm -f "$log"
[ "$bytes" -gt 0 ] || { echo "nothing captured on $PORT" >&2; exit 1; }
