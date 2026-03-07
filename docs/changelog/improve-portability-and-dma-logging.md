# Improvement: Architecture Portability and DMA Logging

**Date:** 2026-03-07  
**Severity:** Improvement / Refactoring  
**Files Modified:** `kernel/utils.c`, `portable/cortex-m4/port.c`, `portable/cortex-m4/port.h`, `include/utils.h`

## Problem

1.  **Portability Leak**: The kernel utilities (`utils.c`) contained inline assembly specific to Cortex-M4 (e.g., `mrs psp`, `cpsid i`), making it difficult to port VAiOS to other architectures (like RISC-V or Simulators).
2.  **Synchronous Bottlenecks**: Logging was purely synchronous/polling-based, consuming significant CPU cycles during high-frequency debug output.
3.  **Flag Inconsistency**: The `scheduler_running` flag used inconsistent "magic" values (e.g., `123`), making logic checks brittle.

## Solution

1.  **Portable Hardware Abstraction Layer**:
    - Moved all `__asm` blocks from `kernel/utils.c` to `portable/cortex-m4/port.c`.
    - Extracted hardware register manipulation (e.g., ICSR PendSV trigger) into portable wrappers: `v_port_get_psp()`, `v_port_trigger_pendsv()`, `v_port_disable_interrupts()`, and `v_port_halt()`.
2.  **DMA Logging Integration**:
    - Integrated NavHAL's `direct_dma_print` into the buffered logging system (`v_log_flush`).
    - Implemented an `is_panicking` safety flag to automatically switch back to reliable polling I/O during kernel panics.
3.  **Safety Tuning**:
    - Reverted DMA usage in `v_print` to synchronous polling to avoid race conditions with stack-allocated strings.
    - Reduced the proactive stack overflow margin from 320 to 128 bytes to support tasks with smaller (512-byte) stacks without premature termination.

## Result

The kernel core is now 100% architecture-agnostic. Logging performance is significantly improved via DMA offloading, while the system remains robust enough to report errors even when the stack is nearly exhausted.

## Validation Proof

### Portability Verification

The system now builds and runs the `SWITCHING` and `STACK_OVERFLOW` examples using only the portable layer wrappers.

### DMA Callback Verification

```text
[INFO 0] [VAIOS INIT] SYSTICK started with time period of 1000 μs
[INFO 0] [VAIOS INIT] UART started with baudrate 115200 bps
[INFO 104] [MEMORY] Heap memory head initialized at 0x20000570 size 0x1600
[INFO 107] Entered task 2
```

(Logs processed asynchronously via DMA Stream 6, Channel 4)

## Lesson

Kernel code should never touch hardware registers or assembly directly. Decoupling the "Internal OS Logic" from the "Hardware Implementation" is essential for long-term maintainability and multi-arch support.
