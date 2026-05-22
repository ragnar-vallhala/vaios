# Improvement: Hardened Memory Protection and Panic System

**Date:** 2026-03-07  
**Severity:** Improvement / Safety Enhancement  
**Files Modified:** `kernel/utils.c`, `kernel/memory.c`, `kernel/task.c`, `include/task.h`, `include/vaios_config_default.h`, `portable/cortex-m4/port.c`

## Problem

The previous panic system was rudimentary and vulnerable to recursive failures. Specifically:

1.  **Silent Overflows**: Task stack overflows were not detected proactively. If a task overflowed its stack, it would either silently corrupt adjacent memory (like TCBs or Heap headers) or trigger a delayed HardFault that was difficult to trace back to the offending task.
2.  **Panic Fragility**: The panic reporter itself used local stack buffers (via `vaprint_fmt`). If a panic was triggered due to a stack overflow, the reporter would often cause a second overflow, leading to a system hang or a secondary HardFault.
3.  **Late Detection**: Memory exhaustion was only detected when an allocation failed, which might be too late for some safety-critical systems.

## Solution

Implemented a multi-layered defense-in-depth strategy for memory safety:

1.  **Proactive Stack Monitoring**:
    - Added a check in `v_log()` that uses the live `PSP` register.
    - Triggers a `v_panic()` if the stack pointer comes within 320 bytes of the task's memory boundary.
    - Uses a `scheduler_running` guard and `TCB_MAGIC` validation to prevent false positives during boot or after corruption.

2.  **Stack-Safe Panic Reporting**:
    - Created `v_panic_vprintf` which uses **global static buffers** instead of stack-allocated arrays.
    - Ensures the final "death cry" of the system works even if the task stack is at 0 bytes.

3.  **Heap Watermark Monitoring**:
    - Implemented `HEAP_WATERMARK_THRESHOLD`.
    - `v_malloc()` now triggers a panic if total heap usage exceeds this threshold, allowing the system to fail-safe before total exhaustion.

4.  **HardFault Integration**:
    - Modified the `HardFault_Handler` to automatically transition to a `v_panic()` state after dumping diagnostic registers.

## Result

Stack overflows are now caught accurately and reported with the correct Task ID and real-time Stack Pointer. The system remains stable (halted, but informative) during memory exhaustion and severe stack pressure, preventing silent data corruption.

## Validation Proof

### Stack Overflow Detection

```text
[INFO 104] Starting Stack Overflow Example
[INFO 106] Stack overflow task started
[INFO 207] Recursion depth: 1
...
[INFO 228] Recursion depth: 15

[KERNEL PANIC] at /home/ragnar/Documents/Drone/vaios/kernel/utils.c:637
Reason: Stack overflow detected in task 2 during log! SP: 0x20000f08, Limit: 0x20000f48
System Halted.
```

### Heap Watermark Panic

```text
[INFO 104] Starting Memory Leak Example
[INFO 105] Heap Size: 90112 bytes
[INFO 106] Leaking task started
[INFO 463] Allocated 8 KB
...
[INFO 4135] Allocated 80 KB

[KERNEL PANIC] at /home/ragnar/Documents/Drone/vaios/kernel/memory.c:125
Reason: Heap watermark exceeded! Used: 89568, Limit: 89088
System Halted.
```

## Lesson

Kernel diagnostic tools (Panics, Logs) must be designed to work in the very failure modes they are meant to detect. For a stack overflow detector, this means the reporter must not rely on the stack.
