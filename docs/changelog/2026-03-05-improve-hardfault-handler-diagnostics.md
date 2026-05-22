# Fix: HardFault Handler — Added SCB Fault Status Registers

**Date:** 2026-03-05  
**Severity:** Improvement — enhanced crash diagnostics for faster root-cause identification  
**Files Modified:** `portable/cortex-m4/port.c`

## Problem

The original HardFault handler only printed `PC`, `LR`, `R0`, `SP`, and
`EXC_RETURN`. This was insufficient to distinguish between:
- **INVSTATE** (executing from an invalid state — e.g. PC=0)
- **MEMFAULT** (MPU or null pointer dereference)
- **BUSFAULT** (unaligned access, invalid bus transaction)
- **USAGEFAULT** (undefined instruction, divide by zero)

Without the Configured Fault Status Register (CFSR), the exact fault class had
to be guessed from the PC value alone.

## Fix

Added reads of SCB fault registers in `hardfault_handler_c()`:

```c
uint32_t cfsr  = *(volatile uint32_t *)0xE000ED28; // CFSR (MemFault+BusFault+UsageFault)
uint32_t hfsr  = *(volatile uint32_t *)0xE000ED2C; // HardFault Status Register
uint32_t mmfar = *(volatile uint32_t *)0xE000ED34; // MemManage Fault Address
uint32_t bfar  = *(volatile uint32_t *)0xE000ED38; // BusFault Address Register

v_print("CFSR: "); print_hex_blocking(cfsr);
v_print("HFSR: "); print_hex_blocking(hfsr);
v_print("MMFAR: "); print_hex_blocking(mmfar);
v_print("BFAR: "); print_hex_blocking(bfar);
```

### CFSR Bit Fields Reference

| Bits    | Field     | Meaning                              |
|---------|-----------|--------------------------------------|
| [7:0]   | MMFSR     | MemManage fault status               |
| [15:8]  | BFSR      | BusFault status                      |
| [31:16] | UFSR      | UsageFault status                    |
| [17]    | INVSTATE  | `1` = executed with invalid EPSR state (e.g. PC=0 → ARM mode) |
| [16]    | UNDEFINSTR| `1` = undefined instruction          |

The crash in this session showed `CFSR: 0x00020000` = bit 17 set = **INVSTATE**,
confirming the CPU tried to execute from `PC=0` (null pointer) in ARM (non-Thumb)
mode derived from a corrupted LR.

## Result

Root-cause identification time is significantly reduced with the detailed
fault register dump.
