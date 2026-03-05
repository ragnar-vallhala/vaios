# Fix: Heap Block Alignment — `Heap_Mem_Block` Padding to 16 Bytes

**Date:** 2026-03-05  
**Severity:** High — caused unaligned stack allocations and intermittent HardFaults  
**Files Modified:** `include/memory.h`

## Problem

The `Heap_Mem_Block` header struct was 12 bytes in size (unpadded), meaning all
heap allocations started at 12-byte-offset addresses. On Cortex-M4 with FPU
enabled, stack frames require **8-byte alignment**. Because every allocation
was preceded by a 12-byte header, user data addresses were 4-byte aligned but
not 8-byte aligned, violating the ARM ABI requirement for double-word and
FP register saves.

This caused sporadic bus faults and spurious data corruption whenever FPU
context saves pushed `s16-s31` to an unaligned stack address.

## Fix

Added `uint32_t _pad` to `Heap_Mem_Block` to pad it to exactly 16 bytes:

```c
typedef struct Heap_Mem_Block {
  uint32_t size;
  uint8_t  status;
  uint32_t magic_number;
  uint8_t  _pad[3];   // pad to 16 bytes for 8-byte alignment
} Heap_Mem_Block;
```

With a 16-byte header, every allocation (sized as a multiple of 4) starts at
a 16-byte-aligned address, satisfying the 8-byte alignment requirement.

## Result

FPU-related memory faults eliminated. FPU context-save benchmark passes
cleanly.
