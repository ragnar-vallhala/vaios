#!/usr/bin/env bash
# =============================================================================
# tools/flash.sh — vaios CLI flasher
#
# Probe the connected debug probe(s), resolve the target board, build a vaios
# example firmware, flash it, and reset the core so the image actually starts.
#
# Modelled on nav's upload pipeline (../nav): detection is data-driven (a small
# chipid->board table below, mirroring nav's data/boards.json), flashing is
# objcopy -> <flasher> write -> reset, and the reset is explicit because
# st-flash leaves the core halted on boards whose NRST isn't wired to the probe.
#
# Selecting one board when several are connected:
#   --serial <hex>   pick an STM32 ST-Link by its probe serial (st-flash --serial).
#                    Run --list to see every probe's serial.
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
#   -s, --serial <hex>  target ST-Link with this serial (when several are connected)
#   -p, --port <path>   serial port to monitor / (future) AVR flash target
#   -c, --clean         wipe the build dir before configuring (fresh build)
#   -b, --build-only    build the firmware but do not flash
#   -n, --no-reset      do not reset the core after flashing
#   -m, --monitor       open a serial monitor after flashing
#       --baud <n>      monitor baud rate (default: 115200)
#   -a, --addr <hex>    override the flash base address (default: from board table)
#   -h, --help          show this help
#
# Examples:
#   tools/flash.sh fifo_test                       # build + flash + reset
#   tools/flash.sh --list                          # probe what's connected
#   tools/flash.sh -s 0668FF33 stack_overflow      # pick one of several ST-Links
#   tools/flash.sh -m -p /dev/ttyACM1 --baud 115200 uart
# =============================================================================
set -euo pipefail

# --- Locate the repo root (this script lives in <root>/tools) ----------------
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
ROOT="$(cd -- "${SCRIPT_DIR}/.." >/dev/null 2>&1 && pwd)"
BUILD_DIR="${ROOT}/build"

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

# --- Board registry: chipid -> "name|flash_base" -----------------------------
# STM32 parts all boot from 0x08000000; the table exists so we can name the
# detected part and refuse an unknown probe rather than blindly flashing. Add a
# row to support a new chip (mirrors adding a board to nav's boards.json).
declare -A BOARD_BY_CHIPID=(
  [0x433]="STM32F401xD/xE|0x08000000"
  [0x431]="STM32F411xC/xE|0x08000000"
  [0x413]="STM32F405/407/415/417|0x08000000"
  [0x419]="STM32F42x/43x|0x08000000"
  [0x421]="STM32F446|0x08000000"
  [0x423]="STM32F401xB/xC|0x08000000"
)
DEFAULT_FLASH_ADDR="0x08000000"

# --- Serial port enumeration (mirrors nav's find_serial_ports) ---------------
find_serial_ports() {
  local p
  for p in /dev/ttyACM* /dev/ttyUSB*; do
    [[ -e "$p" ]] && printf '%s\n' "$p"
  done | sort
}

# --- ST-Link probe -----------------------------------------------------------
# Emits one line per connected probe: "serial|chipid|dev-type". A new "serial:"
# line marks the start of each probe block in st-info --probe output.
probe_stlink() {
  st-info --probe 2>/dev/null | awk '
    /serial:/   { if (have) print s"|"c"|"d; s=$2; c=""; d=""; have=1 }
    /chipid:/   { c=$2 }
    /dev-type:/ { d=$2 }
    END         { if (have) print s"|"c"|"d }
  '
}

# --- Buildable example names, parsed from examples/CMakeLists.txt -------------
# Kept in sync with the build (nothing hardcoded) — same idea as nav resolving
# targets from data rather than a duplicated list.
list_examples() {
  grep -oE 'VAIOS_EXAMPLE STREQUAL "[A-Z0-9_]+"' "${ROOT}/examples/CMakeLists.txt" \
    | sed -E 's/.*"([A-Z0-9_]+)"/\1/' | sort
}

board_name_for() {  # chipid -> friendly name or "unknown"
  local e="${BOARD_BY_CHIPID[$1]:-}"; [[ -n "$e" ]] && printf '%s' "${e%%|*}" || printf 'unknown'
}

# --- Arg parsing -------------------------------------------------------------
EXAMPLE=""
DO_CLEAN=0; BUILD_ONLY=0; NO_RESET=0; DO_MONITOR=0; LIST_ONLY=0
BAUD="115200"; ADDR_OVERRIDE=""; SERIAL=""; PORT_OVERRIDE=""

usage() { sed -n '2,48p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)          usage; exit 0 ;;
    -l|--list)          LIST_ONLY=1; shift ;;
    --list-examples)    list_examples; exit 0 ;;
    -s|--serial)        SERIAL="${2:?--serial needs a value}"; shift 2 ;;
    -p|--port)          PORT_OVERRIDE="${2:?--port needs a value}"; shift 2 ;;
    -c|--clean)         DO_CLEAN=1; shift ;;
    -b|--build-only)    BUILD_ONLY=1; shift ;;
    -n|--no-reset)      NO_RESET=1; shift ;;
    -m|--monitor)       DO_MONITOR=1; shift ;;
    --baud)             BAUD="${2:?--baud needs a value}"; shift 2 ;;
    -a|--addr)          ADDR_OVERRIDE="${2:?--addr needs a value}"; shift 2 ;;
    -*)                 die "Unknown option: $1  (see --help)" ;;
    *)                  [[ -z "$EXAMPLE" ]] || die "Only one example may be given (got '$EXAMPLE' and '$1')"; EXAMPLE="$1"; shift ;;
  esac
done

# --- Required tools ----------------------------------------------------------
for t in st-info st-flash arm-none-eabi-objcopy cmake; do
  command -v "$t" >/dev/null 2>&1 || die "required tool not found on PATH: $t"
done

# --- Detect hardware ---------------------------------------------------------
mapfile -t PROBES < <(probe_stlink)
mapfile -t PORTS  < <(find_serial_ports)

if [[ "$LIST_ONLY" == "1" ]]; then
  step "Detected hardware"
  if [[ ${#PROBES[@]} -eq 0 ]]; then
    info "ST-Link: none found"
  else
    info "ST-Link probes: ${#PROBES[@]}"
    local_i=0
    for rec in "${PROBES[@]}"; do
      IFS='|' read -r s c d <<<"$rec"
      info "  [$local_i] serial ${s}  chipid ${c:-?} ($(board_name_for "${c}") / ${d:-unknown})"
      local_i=$((local_i+1))
    done
    [[ ${#PROBES[@]} -gt 1 ]] && info "select one with:  --serial <serial>"
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
SEL_SERIAL=""; SEL_CHIPID=""; SEL_DEVTYPE=""
if [[ "$BUILD_ONLY" == "0" ]]; then
  [[ ${#PROBES[@]} -gt 0 ]] || die "no ST-Link found — connect the board (or use --build-only)."
  SEL_REC=""
  if [[ -n "$SERIAL" ]]; then
    matches=()
    for rec in "${PROBES[@]}"; do
      s="${rec%%|*}"
      [[ "$s" == "$SERIAL" || "$s" == "$SERIAL"* ]] && matches+=("$rec")
    done
    case ${#matches[@]} in
      0) die "no connected ST-Link matches serial '$SERIAL'. Run --list to see serials." ;;
      1) SEL_REC="${matches[0]}" ;;
      *) die "serial '$SERIAL' is ambiguous (${#matches[@]} probes match). Give more digits." ;;
    esac
  elif [[ ${#PROBES[@]} -eq 1 ]]; then
    SEL_REC="${PROBES[0]}"
  else
    warn "multiple ST-Link probes connected — pick one with --serial <serial>:"
    for rec in "${PROBES[@]}"; do IFS='|' read -r s c d <<<"$rec"; info "  ${s}  ($(board_name_for "$c") / ${d:-?})"; done
    exit 1
  fi
  IFS='|' read -r SEL_SERIAL SEL_CHIPID SEL_DEVTYPE <<<"$SEL_REC"
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
BIN="${BUILD_DIR}/examples/main.bin"
[[ -f "$ELF" ]] || die "build produced no firmware at ${ELF} (did the example link?)"
arm-none-eabi-objcopy -O binary "$ELF" "$BIN"
ok "Built $(basename "$BIN") ($(stat -c%s "$BIN") bytes)"

if [[ "$BUILD_ONLY" == "1" ]]; then
  ok "Build-only requested — not flashing."
  exit 0
fi

# --- Resolve flash address ---------------------------------------------------
FLASH_ADDR="$DEFAULT_FLASH_ADDR"
if [[ -n "$ADDR_OVERRIDE" ]]; then
  FLASH_ADDR="$ADDR_OVERRIDE"
elif [[ -n "${BOARD_BY_CHIPID[$SEL_CHIPID]:-}" ]]; then
  entry="${BOARD_BY_CHIPID[$SEL_CHIPID]}"; FLASH_ADDR="${entry##*|}"
fi

# --- Flash -------------------------------------------------------------------
step "Flashing ${EXAMPLE_UC} -> ${SEL_DEVTYPE:-target} (serial ${SEL_SERIAL}) @ ${FLASH_ADDR}"
st-flash --serial "$SEL_SERIAL" --connect-under-reset write "$BIN" "$FLASH_ADDR"
ok "Flashed."

# --- Reset (st-flash halts the core after write; start the new image) --------
if [[ "$NO_RESET" == "0" ]]; then
  step "Resetting target"
  if st-flash --serial "$SEL_SERIAL" reset >/dev/null 2>&1; then
    ok "Target reset — running new firmware."
  else
    warn "reset failed — press the board's RESET button to start the image."
  fi
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
