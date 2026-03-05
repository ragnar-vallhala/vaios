# VAiOS Benchmark Report — 5 March 2026

## Platform

| Field | Value |
|---|---|
| Board | STM32F401RE (Nucleo-64) |
| CPU | Cortex-M4 @ **84 MHz** (PLL: HSI/16 × 336 / 4) |
| FPU | Enabled (hard-float ABI, lazy stacking) |
| DMA | Enabled (`_DMA_ENABLED`, DMA-backed UART logging) |
| RTOS | VAiOS v0.1.0 |
| Build | Release (`-O2`, `-mfpu=fpv4-sp-d16`, `-mfloat-abi=hard`) |
| SysTick | 1 ms period |
| UART | USART2 @ 115200 bps |

---

## Results Summary

**14 PASS · 0 FAIL · 0 TIMEOUT · 0 SKIP**

| # | Benchmark | Status | Duration | Throughput |
|---|---|:---:|---:|---:|
| 1 | FPU: sinf throughput | ✅ PASS | 4 ms | 250 000 ops/s |
| 2 | FPU: sqrtf throughput | ✅ PASS | 3 ms | 333 333 ops/s |
| 3 | FPU: multi-task context-save | ✅ PASS | 4 ms | 250 000 ops/s |
| 4 | DMA: M2M 64 B (looped) | ✅ PASS | 20 ms | ~2 000 KB/s |
| 5 | DMA: M2M 4 KB (looped) | ✅ PASS | 20 ms | ~2 000 KB/s |
| 6 | DMA: concurrent streams (looped) | ✅ PASS | 21 ms | ~1 904 KB/s |
| 7 | TASK: context switch rate | ✅ PASS | 212 ms | 3 773 sw/s |
| 8 | TASK: priority preemption | ✅ PASS | 65 ms | — |
| 9 | TASK: delay accuracy | ✅ PASS | 101 ms | err = 1 % |
| 10 | IPC: semaphore ping-pong | ✅ PASS | 31 ms | 32 258 trips/s |
| 11 | IPC: mutex shared counter | ✅ PASS | 69 ms | — |
| 12 | MEM: alloc/free throughput | ✅ PASS | 97 ms | 6 185 ops/s |
| 13 | MEM: fragmentation resilience | ✅ PASS | — | heap coalesces |
| 14 | STRESS: all subsystems concurrent | ✅ PASS | 8 004 ms | — |

---

## Notable Observations

### FPU
- `sinf` completes 1 000 ops in 4 ms → **250 kops/s**
- `sqrtf` completes 1 000 ops in 3 ms → **333 kops/s** (faster than sin, as expected)
- Multi-task FPU context-save verified correct: `sine_sum=30958`, `sqrt_sum=655`

### DMA
- DMA M2M transfers complete in < 1 ms per burst (well under one SysTick tick), so benchmarks use a **20 ms timed loop** to produce measurable results.
- All transfers verified byte-for-byte; no corruption observed.
- Measured throughput  ~2 MB/s is consistent with STM32F4 AHB bus overhead for 8-bit M2M DMA.

### Tasks
- Context switch rate **3 773 sw/s** (212 ms / 800 switches).
- Delay accuracy **1 %** error (requested 100 ms, measured 101 ms) — within one SysTick period.

### IPC
- Semaphore ping-pong **32 258 trips/s** over 31 ms.
- Mutex test: 2 000 increments = 2 000 expected — no race condition.

### Memory
- `small` alloc/free (32 B × 1 000): 14 ms
- `medium` alloc/free (128 B × 500): 24 ms
- `large` alloc/free (1 024 B × 100): 59 ms
- Zero allocation failures; heap coalesces fragmented blocks correctly.

### Stress (8 s window)
- 200 DMA transfers, 0 corruptions
- FPU accumulator non-zero and sane
- IPC 65 trips, MEM 0 fails
- All subsystems **PASS** concurrently

---

## Bugs Fixed in This Session

| Bug | Root Cause | Fix |
|---|---|---|
| System hung after benchmarks | `uart2_write_dma()` called `dma_init()` on every flush; `dma_init()` has a blocking `while(EN)` spin that froze `SysTick_Handler` | `dma_init()` now called only once; subsequent flushes reload `M0AR`/`NDTR` directly |
| Spurious DMA TC interrupt | Stale TC flag was present when NVIC was armed, firing ISR immediately and clearing the log lock prematurely | `dma_clear_flags()` called **before** `hal_enable_interrupt()` |
| `v_log()` hard lockup on stuck DMA TC | Unbounded spin-wait in `v_log()` | Bounded spin-guard (200 k iterations) with force-clear fallback |

---

## Log File

[benchmark_84MHz.log](./benchmark_84MHz.log)
