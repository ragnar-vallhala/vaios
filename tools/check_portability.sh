#!/usr/bin/env bash
# tools/check_portability.sh — the portability tripwire.
#
# kernel/ and include/ are the architecture-neutral core: no inline asm, no
# CMSIS/vendor register access, no arch #ifdefs, no memory-mapped I/O. Everything
# hardware-specific belongs in portable/<arch>/ behind the v_port_* facade. This
# script fails if any of those patterns appear in kernel/ or include/, so a
# regression is caught in CI the moment it lands rather than at a future port.
#
# Scope is kernel/ + include/ ONLY. portable/ is where raw access is supposed to
# live; examples/ and tools/ are allowed to be board-specific (poking a known
# address there IS the experiment). Scoped by directory with grep on purpose:
# CodeQL's paths-ignore is silently ignored for a built C/C++ analysis (see the
# header of .github/workflows/codeql.yml), so grep is the only tool that can
# express "everywhere except this directory".
#
# There is deliberately NO suppression file. A hit is a design fix — move the
# code into portable/<arch>/ — not a baseline entry. If a genuine, reviewed
# exception ever arises, add an explicit `-e` exclusion to the relevant check
# below with a comment justifying it, so the reasoning lives in the gate.
#
# Exit: 0 = clean, 1 = a hardware-specific pattern leaked outside portable/.
set -u

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR" || exit 2
SCOPE=(kernel include)
rc=0

# Run one check: label, fix-hint, extended-regex. Any match fails the gate.
check() {
  local label="$1" hint="$2" re="$3"
  local hits
  hits="$(grep -rnE --include='*.c' --include='*.h' "$re" "${SCOPE[@]}" || true)"
  if [ -n "$hits" ]; then
    echo "  FAIL [$label]: $(printf '%s\n' "$hits" | wc -l) hit(s) — $hint"
    printf '%s\n' "$hits" | sed 's/^/    /'
    rc=1
  fi
}

echo "=== portability tripwire: no hardware-specific code in kernel/ or include/ ==="

# Inline assembly — always a port concern (register access, barriers, traps).
check "inline-asm" "move behind a v_port_* wrapper in portable/<arch>/" \
  '(^|[^[:alnum:]_])(__asm|asm[[:space:]]+volatile)'

# Architecture macros / arch #ifdefs. The kernel gates on capability symbols
# (VAIOS_ARCH_HAS_*), never on an arch name. \bCORTEX_M4\b does not match the
# allowed VAIOS_ARCH_CORTEX_M4 (no word boundary after the underscore).
check "arch-macro" "gate on a VAIOS_ARCH_* capability, not an arch name" \
  '\b(__arm__|__AVR__|__thumb__|__thumb2__|__ARM_ARCH|CORTEX_M4)\b'

# Vendor / CMSIS headers included from the portable core. The project's own
# port-facade headers are NOT flagged: include/syscall.h including
# "port_syscall.h" is the sanctioned seam (portable prototypes here, arch impl in
# the port) — the leak would be pulling a *vendor* header directly.
check "vendor-include" "include it in portable/<arch>/, reach it via v_port_*" \
  '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"](avr/|stm32|navhal|cmsis|core_cm)'

# Memory-mapped I/O: an integer literal cast to a pointer. NOT clang-tidy's
# performance-no-int-to-ptr, which is inverted (it fires on casts from integer
# *variables* and exempts constant addresses). Matches all spellings incl.
# `const volatile`, multi-word types, and interior spacing.
check "mmio" "move behind a v_port_* function in portable/<arch>/" \
  '\([[:space:]]*[A-Za-z_][A-Za-z0-9_[:space:]]*\*[[:space:]]*\)[[:space:]]*0[xX][0-9A-Fa-f]'

if [ "$rc" -eq 0 ]; then
  echo "  PASS: kernel/ and include/ are architecture-neutral"
fi
exit "$rc"
