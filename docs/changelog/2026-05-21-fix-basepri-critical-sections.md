# Fix: Critical Sections Masked Every Interrupt (PRIMASK → BASEPRI)

**Date:** 2026-05-21
**Severity:** Critical — ship-blocker for flight; scheduler internals could stall motor PWM / IMU IRQs
**Files Modified:** `portable/cortex-m4/port.c`, `include/vaios_config_default.h`

## Problem

`v_enter_critical()` / `v_exit_critical()` used `cpsid i` / `cpsie i`, which
mask **every** maskable interrupt:

```c
void v_enter_critical(void) {
  __asm volatile("cpsid i" ::: "memory");   // blocks ALL IRQs
  critical_nesting++;
}
```

The scheduler holds critical sections during ready-list manipulation,
wait-queue updates, and allocator walks. With `cpsid i`, motor PWM, IMU EXTI,
and ESC telemetry interrupts all stalled for the full duration of every
critical section — unacceptable latency for a 1 kHz flight control loop.

## Fix

Switched to BASEPRI masking under a `VAIOS_USE_BASEPRI` config flag
(default on):

```c
void v_enter_critical(void) {
  uint32_t pri = MAX_SYSCALL_INTERRUPT_PRIORITY;
  __asm volatile("msr basepri, %0" :: "r"(pri) : "memory");
  critical_nesting++;
}
```

BASEPRI masks only interrupts at or below `MAX_SYSCALL_INTERRUPT_PRIORITY`.
IRQs with a numerically lower priority value (motor PWM, IMU, ESC telemetry)
stay unmaskable — but must not call vaios APIs. `v_port_disable_interrupts()`
keeps `cpsid i` for the panic path. The original behaviour is preserved
behind `VAIOS_USE_BASEPRI=0` for bring-up.

## Result

Hard-real-time interrupts now fire through scheduler internals. The
documented NVIC priority bands: 0–6 unmaskable (no vaios APIs), 7–14 maskable
(`*_from_isr` allowed), 15 for PendSV/SysTick.

## Lesson

An RTOS must never globally disable interrupts for its own bookkeeping. BASEPRI
gives the kernel a critical section while leaving a band of priorities free for
hardware that genuinely cannot wait.
