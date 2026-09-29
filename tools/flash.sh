#!/usr/bin/env bash
# =============================================================================
# tools/flash.sh — vaios CLI flasher
#
# Probe the connected debug probe(s), resolve the target board, build a vaios
# example firmware, flash it, and reset the core so the image actually starts.
#
# Flashing goes through OpenOCD (tools/lib/probe.sh), which programs the ELF
# directly — load addresses come from the image, so there is no board table and
# no separate objcopy or reset step.
#
# Selecting one board when several are connected:
#   --usb-loc <b-p>  pick a probe by the USB port it is plugged into, e.g. 3-2.
#                    Run --list to see what is attached. A serial is NOT usable
#                    here: the bench's ST-Links are clones sharing one serial.
#   --port <path>    pick a board by serial port (e.g. /dev/ttyACM1). Used for the
#                    serial monitor now, and reserved for AVR/Arduino (avrdude -P).
#
# Usage:
#   tools/flash.sh [options] <EXAMPLE>
#
#   <EXAMPLE>            example to flash, case-insensitive (e.g. fifo_test,
#                        FIFO_TEST, stack_overflow). Run --list-examples to see all.
#
# Options:
#   -l, --list          probe hardware (ST-Link probes + serial ports), print, exit
#       --list-examples  list the buildable example names and exit
#   -u, --usb-loc <b-p> target the probe at this USB location (default 3-2)
#   -p, --port <path>   serial port to monitor / (future) AVR flash target
#   -c, --clean         wipe the build dir before configuring (fresh build)
#   -b, --build-only    build the firmware but do not flash
#   -n, --no-reset      do not reset the core after flashing
#   -m, --monitor       open a serial monitor after flashing
#       --baud <n>      monitor baud rate (default: 115200)
#   -h, --help          show this help
#
# Examples:
#   tools/flash.sh fifo_test                       # build + flash + reset
#   tools/flash.sh --list                          # probe what's connected
#   tools/flash.sh -u 3-2 stack_overflow           # pick one of several probes
#   tools/flash.sh -m -p /dev/ttyACM1 --baud 115200 uart
# =============================================================================
set -euo pipefail

# --- Locate the repo root (this script lives in <root>/tools) ----------------
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
ROOT="$(cd -- "${SCRIPT_DIR}/.." >/dev/null 2>&1 && pwd)"
BUILD_DIR="${ROOT}/build"
# shellcheck source=lib/probe.sh
. "${SCRIPT_DIR}/lib/probe.sh"

# --- Pretty output (fall back to plain text when not a TTY) ------------------
if [[ -t 1 ]]; then
  C_RST=$'\033[0m'; C_B=$'\033[1m'; C_GRN=$'\033[32m'; C_YEL=$'\033[33m'
  C_RED=$'\033[31m'; C_CYA=$'\033[36m'
else
  C_RST=""; C_B=""; C_GRN=""; C_YEL=""; C_RED=""; C_CYA=""
fi
step()  { printf '%s==>%s %s%s%s\n'  "$C_CYA" "$C_RST" "$C_B" "$*" "$C_RST"; }
info()  { printf '    %s\n' "$*"; }
ok()    { printf '%s ✓ %s%s\n' "$C_GRN" "$*" "$C_RST"; }
warn()  { printf '%s ! %s%s\n' "$C_YEL" "$*" "$C_RST" >&2; }
die()   { printf '%s ✗ %s%s\n' "$C_RED" "$*" "$C_RST" >&2; exit 1; }

# --- Serial port enumeration (mirrors nav's find_serial_ports) ---------------
find_serial_ports() {
  local p
  for p in /dev/ttyACM* /dev/ttyUSB*; do
    [[ -e "$p" ]] && printf '%s\n' "$p"
  done | sort
}

# --- Probe enumeration (passive) ---------------------------------------------
# Reads sysfs rather than talking to the probes: `st-info --probe` and an
# OpenOCD init both RESET every board they touch, and this runs on --list.
# Emits one line per attached ST-Link: "<bus>-<port>|<product>".
probe_locations() {
  local d loc
  for d in /sys/bus/usb/devices/*/; do
    [ "$(cat "$d/idVendor" 2>/dev/null)" = "0483" ] || continue
    loc="$(basename "$d")"
    case "$loc" in *:*) continue ;; esac   # skip interface nodes
    printf '%s|%s\n' "$loc" "$(cat "$d/product" 2>/dev/null || echo 'ST-Link')"
  done
}

# --- Buildable example names, parsed from examples/CMakeLists.txt -------------
# Kept in sync with the build (nothing hardcoded) — same idea as nav resolving
# targets from data rather than a duplicated list.
list_examples() {
  grep -oE 'VAIOS_EXAMPLE STREQUAL "[A-Z0-9_]+"' "${ROOT}/examples/CMakeLists.txt" \
    | sed -E 's/.*"([A-Z0-9_]+)"/\1/' | sort
}

# --- Arg parsing -------------------------------------------------------------
EXAMPLE=""
DO_CLEAN=0; BUILD_ONLY=0; NO_RESET=0; DO_MONITOR=0; LIST_ONLY=0
BAUD="115200"; PORT_OVERRIDE=""

usage() { sed -n '2,41p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)          usage; exit 0 ;;
    -l|--list)          LIST_ONLY=1; shift ;;
    --list-examples)    list_examples; exit 0 ;;
    -u|--usb-loc)       USB_LOC="${2:?--usb-loc needs a value}"; shift 2 ;;
    -p|--port)          PORT_OVERRIDE="${2:?--port needs a value}"; shift 2 ;;
    -c|--clean)         DO_CLEAN=1; shift ;;
    -b|--build-only)    BUILD_ONLY=1; shift ;;
    -n|--no-reset)      NO_RESET=1; shift ;;
    -m|--monitor)       DO_MONITOR=1; shift ;;
    --baud)             BAUD="${2:?--baud needs a value}"; shift 2 ;;
    -*)                 die "Unknown option: $1  (see --help)" ;;
    *)                  [[ -z "$EXAMPLE" ]] || die "Only one example may be given (got '$EXAMPLE' and '$1')"; EXAMPLE="$1"; shift ;;
  esac
done

# --- Required tools ----------------------------------------------------------
for t in cmake; do
  command -v "$t" >/dev/null 2>&1 || die "required tool not found on PATH: $t"
done
probe_require_tools || die "required tool not found on PATH: openocd"

# --- Detect hardware ---------------------------------------------------------
mapfile -t PROBES < <(probe_locations)
mapfile -t PORTS  < <(find_serial_ports)

if [[ "$LIST_ONLY" == "1" ]]; then
  step "Detected hardware"
  if [[ ${#PROBES[@]} -eq 0 ]]; then
    info "ST-Link: none found"
  else
    info "ST-Link probes: ${#PROBES[@]}"
    for rec in "${PROBES[@]}"; do
      IFS='|' read -r loc prod <<<"$rec"
      info "  usb ${loc}  ${prod}$([[ "$loc" == "$USB_LOC" ]] && echo '   <- default')"
    done
    info "select one with:  --usb-loc <bus>-<port>"
  fi
  if [[ ${#PORTS[@]} -eq 0 ]]; then info "Serial:  no ttyACM*/ttyUSB* ports"; else
    info "Serial ports: ${PORTS[*]}"; fi
  exit 0
fi

# --- Resolve the example name (case-insensitive) -----------------------------
[[ -n "$EXAMPLE" ]] || die "no example given. Try: $(basename "$0") --list-examples"
EXAMPLE_UC="${EXAMPLE^^}"
if ! list_examples | grep -qx "$EXAMPLE_UC"; then
  die "unknown example '$EXAMPLE'. Run '$(basename "$0") --list-examples' for the list."
fi

# --- Select the target ST-Link (before the build, so a bad --serial fails fast)
# --serial picks one out of several (exact match, or a unique prefix so a short
# handle like -s 0668FF33 works). With no --serial: exactly one probe is used,
# zero or many is an error asking the caller to pick. Skipped for --build-only,
# which needs no hardware.
if [[ "$BUILD_ONLY" == "0" ]]; then
  [[ ${#PROBES[@]} -gt 0 ]] || die "no ST-Link found — connect the board (or use --build-only)."
  probe_present || {
    warn "no ST-Link at USB location ${USB_LOC}; attached:"
    for rec in "${PROBES[@]}"; do IFS='|' read -r loc prod <<<"$rec"; info "  ${loc}  ${prod}"; done
    die "pick one with --usb-loc <bus>-<port>."
  }
fi

# --- NavHAL Kconfig needs $srctree pointing at the submodule ------------------
# The main build fails resolving NavHAL's Kconfig without it; export it here so
# a fresh terminal works without the caller having to remember (see memory
# navhal-build-needs-srctree).
export srctree="${ROOT}/extern/NavHAL"

# --- Configure + build -------------------------------------------------------
if [[ "$DO_CLEAN" == "1" ]]; then
  step "Cleaning build dir"
  rm -rf "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"

step "Configuring (example: ${EXAMPLE_UC})"
cmake -S "$ROOT" -B "$BUILD_DIR" \
  -DNAVHAL=ON -DEXAMPLES=ON -DVAIOS_EXAMPLE="${EXAMPLE_UC}" >/dev/null

step "Building"
cmake --build "$BUILD_DIR" -j"$(nproc)"

ELF="${BUILD_DIR}/examples/main"
[[ -f "$ELF" ]] || die "build produced no firmware at ${ELF} (did the example link?)"
ok "Built $(basename "$ELF") ($(stat -c%s "$ELF") bytes)"

# Follow the family this build was configured for, rather than the F4 default.
probe_target_from_build "$BUILD_DIR"

if [[ "$BUILD_ONLY" == "1" ]]; then
  ok "Build-only requested — not flashing."
  exit 0
fi

# --- Flash ------------------------------------------------------------------
# OpenOCD programs the ELF and starts it: `verify` reads the image back, and
# `reset` leaves the core running, so there is no separate reset step.
step "Flashing ${EXAMPLE_UC} -> probe at ${USB_LOC}"
probe_flash "$ELF" /tmp/flash_sh.log || {
  tail -8 /tmp/flash_sh.log >&2
  die "flash failed — hold RESET, re-run, release when writing starts."
}
ok "Flashed and running."

if [[ "$NO_RESET" == "1" ]]; then
  warn "--no-reset: OpenOCD always starts the image after programming."
fi

# --- Optional serial monitor -------------------------------------------------
if [[ "$DO_MONITOR" == "1" ]]; then
  PORT="$PORT_OVERRIDE"
  if [[ -z "$PORT" ]]; then
    if [[ ${#PORTS[@]} -eq 0 ]]; then
      warn "no serial port found to monitor."
    elif [[ ${#PORTS[@]} -gt 1 ]]; then
      warn "multiple serial ports (${PORTS[*]}); choose one with --port <path>."
    else
      PORT="${PORTS[0]}"
    fi
  fi
  if [[ -n "$PORT" ]]; then
    [[ -e "$PORT" ]] || die "serial port '$PORT' does not exist."
    step "Monitoring ${PORT} @ ${BAUD}  (picocom: Ctrl-A Ctrl-X to quit)"
    if command -v picocom >/dev/null 2>&1; then
      exec picocom -b "$BAUD" "$PORT"
    elif command -v screen >/dev/null 2>&1; then
      exec screen "$PORT" "$BAUD"
    else
      stty -F "$PORT" "$BAUD" raw -echo   # minimal fallback
      exec cat "$PORT"
    fi
  fi
fi
