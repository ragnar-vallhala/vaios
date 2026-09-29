# =============================================================================
# tools/lib/probe.sh — talk to the debug probe, selected by USB port.
#
# Source this; it defines probe_present / probe_flash / probe_reset /
# probe_gdbserver. Every hardware script goes through here so there is one
# place that knows how to reach a board.
#
# It uses OpenOCD and selects the adapter by USB LOCATION, not by serial. Both
# ST-Links on this bench are clones that report the SAME serial descriptor, so
# they collapse into one entry: st-flash --serial cannot tell them apart and
# will happily write firmware to the wrong MCU. A USB location is per-port and
# unambiguous.
#
# Environment:
#   USB_LOC=<bus>-<port>   which probe; default 3-2 (the NAVIXSM-F401RE).
#                          `lsusb -t`, or the idVendor 0483 entry under
#                          /sys/bus/usb/devices/, gives the location.
#   OPENOCD_TARGET=<cfg>   target script. Default target/stm32f4x.cfg, but a
#                          script that knows its build dir should call
#                          probe_target_from_build to follow the family it was
#                          configured for. Setting this pins it.
#   OPENOCD_IFACE=<cfg>    interface script; default interface/stlink.cfg
#   GDB_PORT=<n>           gdb server port for probe_gdbserver; default 3333
# =============================================================================

USB_LOC="${USB_LOC:-3-2}"
OPENOCD_IFACE="${OPENOCD_IFACE:-interface/stlink.cfg}"
# Remember whether the caller pinned the target before defaulting it, so
# probe_target_from_build can tell "not set" from "deliberately chosen".
_PROBE_TARGET_PINNED=${OPENOCD_TARGET:+1}
OPENOCD_TARGET="${OPENOCD_TARGET:-target/stm32f4x.cfg}"
GDB_PORT="${GDB_PORT:-3333}"

# probe_target_from_build <build-dir> — point OpenOCD at the family this build
# was configured for, instead of the F4 default.
#
# NavHAL's Kconfig already resolved board -> family and wrote it to the build's
# config.cmake, and OpenOCD names its STM32 scripts target/<family>x.cfg. So
# this is a derivation, not a table of boards: stm32f4 -> target/stm32f4x.cfg,
# stm32f7 -> target/stm32f7x.cfg. An explicit OPENOCD_TARGET wins.
#
# It matters as soon as more than one family is on the bench. The F4 script
# cannot read an F7's id ("device id" never appears and probe_chipid returns
# nothing), and flashing through the wrong family script is not something to
# discover by watching a board fail to come up.
#
# Quiet no-op for a build dir with no config.cmake (a standalone NAVHAL=OFF
# build, or a path that is not a build dir): the default stands, and whatever
# is attempted next reports its own failure.
probe_target_from_build() {
  local cfg="${1:-}/config.cmake" family
  if [ -z "${_PROBE_TARGET_PINNED:-}" ] && [ -f "$cfg" ]; then
    family=$(sed -nE 's/^set\(CONFIG_FAMILY "([^"]+)".*/\1/p' "$cfg" | head -1)
    case "$family" in
      stm32*) OPENOCD_TARGET="target/${family}x.cfg" ;;
    esac
  fi
  return 0
}

probe_require_tools() {
  command -v openocd >/dev/null || {
    echo "missing required tool: openocd" >&2; return 2; }
}

# The adapter-selecting preamble every invocation shares. `adapter usb location`
# has to come after the interface script, which is what selects the driver.
_probe_argv() {
  printf '%s\n' -f "$OPENOCD_IFACE" -c "adapter usb location $USB_LOC" \
                 -f "$OPENOCD_TARGET"
}

# Is a probe attached at USB_LOC? Read from sysfs, NOT by talking to it:
# `st-info --probe` and an OpenOCD init both RESET every board they touch, and
# this gets called while another board may be mid-run.
probe_present() {
  local d v
  for d in /sys/bus/usb/devices/"$USB_LOC"/; do
    [ -e "$d/idVendor" ] || continue
    v=$(cat "$d/idVendor" 2>/dev/null)
    [ "$v" = "0483" ] && return 0
  done
  return 1
}

# probe_flash <elf-or-hex> — write and start it. Takes the ELF directly; no
# objcopy step, and `verify` reads it back rather than trusting the write.
probe_flash() {
  local image="$1" log="${2:-/tmp/openocd_flash.log}"
  mapfile -t argv < <(_probe_argv)
  openocd "${argv[@]}" -c "program \"$image\" verify reset exit" \
    >"$log" 2>&1
}

# probe_reset — restart whatever is already in flash.
probe_reset() {
  mapfile -t argv < <(_probe_argv)
  openocd "${argv[@]}" -c "init" -c "reset run" -c "exit" \
    >/dev/null 2>&1
}

# probe_gdbserver [logfile] — start a gdb server on $GDB_PORT in the background
# and echo its PID. It does NOT reset the target: OpenOCD halts on gdb attach
# but leaves RAM alone, which matters when the thing being read IS RAM (the
# kernel log ring). st-util's default behaviour used to wipe it.
probe_gdbserver() {
  local log="${1:-/tmp/openocd_gdb.log}"
  mapfile -t argv < <(_probe_argv)
  openocd "${argv[@]}" -c "gdb_port $GDB_PORT" -c "init" >"$log" 2>&1 &
  echo $!
}

# probe_chipid — the STM32 device id (e.g. 0x433) of whatever is at USB_LOC, or
# empty if it could not be read. This one DOES touch the board (OpenOCD has to
# init the DAP), so call it only when about to flash anyway.
probe_chipid() {
  local out
  mapfile -t argv < <(_probe_argv)
  # The id comes from the flash driver's probe, not from init, so ask for it;
  # `reset run` afterwards leaves the board running rather than halted.
  out=$(openocd "${argv[@]}" -c "init" -c "flash probe 0" -c "reset run" -c "exit" 2>&1)
  # "Info : device id = 0x10016433" — DBGMCU_IDCODE; the part number is the low
  # 12 bits (0x433 here), the rest is the silicon revision.
  local id
  id=$(printf '%s\n' "$out" | sed -nE 's/.*device id = (0x[0-9a-fA-F]+).*/\1/p' | head -1)
  [ -n "$id" ] || return 1
  printf '0x%03x\n' $(( id & 0xfff ))
}
