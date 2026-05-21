# vaios Flight-Profile Improvement Plan

Companion to `BENCHMARK_PLAN.md`. Drives vaios from the 2026-05-21 measured baseline toward parity with FreeRTOS/Zephyr on the metrics that matter for a 1 kHz drone flight controller.

Every change in this plan is rooted in a **specific measurement** from `results/2026-05-21/report.md` and a **specific source-code defect** in `vaios/vaios/kernel/` or `vaios/vaios/portable/cortex-m4/`. No speculative refactors.

---

## 1. Flight-profile success criteria

The target workload (per `BENCHMARK_PLAN.md` §4.5):

- **1 kHz control loop** with end-to-end jitter < 50 µs (currently 983 µs — basically a full tick late every cycle, **unusable**).
- **IMU ISR → fusion task → PID task pipeline** < 200 µs at p99 (currently 2090 cyc ≈ 25 µs at p50 only; need real ISR-driven test).
- **High-priority IRQs (motor PWM @ 10–20 kHz, IMU @ 1 kHz, ESC telemetry) must never be masked by scheduler internals.** Today they are — see §3.1.
- **Priority inheritance must hold under chained mutex inversion** (sensor bus mutex held by low-prio logger, mid-prio task running, high-prio control loop blocked). Today PI is one-shot and wrong on unlock — see §3.5.
- **Heap p99 under fragmented load** < 5 µs (currently 46 µs at p99 for `malloc_8B` — 100× slower than FreeRTOS).

If any of these fail, the RTOS is not a viable flight-controller base.

---

## 2. Baseline gaps (from `results/2026-05-21/report.md`)

| Metric | vaios | FreeRTOS | Zephyr | Gap factor |
|---|---|---|---|---|
| `ctx_switch_yield` (cyc) | 1316 | **164** | 304 | 8× slower |
| `ctx_switch_fpu` (cyc) | 1466 | **236** | 379 | 6× slower |
| `task_wake_latency` (cyc) | 3347 | **548** | 1196 | 6× slower |
| `sem_give_take` (cyc) | 2104 | 264 | **214** | 10× slower |
| `mutex_lock_unlock` (cyc) | 2490 | 333 | **266** | 9× slower |
| `mutex_pi_basic` (cyc) | 752623 | 243186 | **125918** | 6× slower |
| `malloc_8B` (cyc) | 29000 | **283** | 369 | 102× slower |
| `malloc_64B` (cyc) | 29500 | **279** | 369 | 105× slower |
| `malloc_fragmented` (cyc) | 3867 | 1257 | **461** | 8× slower |
| `delay_accuracy_5ms` (µs) | 6000 | **5000** | 6000 | 1000 µs overshoot on 5 ms |
| `control_loop_1khz_jitter` (µs) | 983 | **0** | 0 | full-tick error per cycle |
| `flash_typical` (bytes) | 78542 | **24757** | 41076 | 3× bigger |

Three classes of problem:

1. **Hot-path logging** — `v_log()` in malloc/free/scheduler paths inflates everything by orders of magnitude. Removing it likely fixes ~half the gaps with no algorithmic change.
2. **Algorithmic** — PRIMASK critical sections, FIFO wait queues, one-shot PI, O(n) malloc/free.
3. **Latent correctness** — delayed-wake is lazy (next scheduler tick rather than IRQ-driven), so periodic tasks are systematically late.

---

## 3. Defects to fix, by impact

### 3.1 PRIMASK → BASEPRI critical sections — **CORRECTNESS, ship-blocker for flight**

**Where:** `vaios/vaios/portable/cortex-m4/port.c:11-21`
```c
void v_enter_critical(void) {
  __asm volatile("cpsid i" ::: "memory");   // BLOCKS ALL IRQs including motor PWM
  critical_nesting++;
}
```

**Why this is fatal for flight:** every scheduler enter/exit path masks every IRQ. Motor PWM, IMU, ESC telemetry all stall for the duration of the critical section. The scheduler holds critical sections during ready-list manipulation, wait-queue updates, allocator walks. With the current O(n) allocator, a single malloc during a critical section can stall motor PWM for tens of microseconds. **You will lose stability margin on the control loop.**

**Fix:** switch to BASEPRI masking, identical to FreeRTOS:
```c
#define MAX_SYSCALL_INTERRUPT_PRIORITY (5 << 4)   /* 4 NVIC bits left-aligned */

void v_enter_critical(void) {
    uint32_t pri = MAX_SYSCALL_INTERRUPT_PRIORITY;
    __asm volatile("msr basepri, %0" :: "r"(pri) : "memory");
    critical_nesting++;
}
void v_exit_critical(void) {
    if (--critical_nesting == 0)
        __asm volatile("msr basepri, %0" :: "r"(0) : "memory");
}
```

Then assign priority levels:
- Motor PWM, IMU EXTI, ESC telemetry: NVIC priority 0–4 (above syscall ceiling). Never masked. Cannot call vaios APIs.
- DMA tx-done, UART, lower-priority sensors: NVIC priority 5–14. Masked by critical sections. May call `*_from_isr` APIs.
- PendSV, SysTick: NVIC priority 15 (lowest).

**Measurable impact:** `kernel_critical_section_max` becomes a finite bounded number rather than "everything blocked". Re-introduce that metric (it's in `BENCHMARK_PLAN.md` §4.1 but currently SKIPped on vaios).

**Estimated effort:** 1 day. Touches `port.c`, every IRQ priority registration in NavHAL, and adds a new `port_init_nvic()` called at boot.

---

### 3.2 Strip `v_log()` from every hot path — **fixes ~half the latency gaps**

**Where:**
- `kernel/memory.c` lines 23, 46, 70, 85, 109, 133, 155, 161, 178, 185, 211 — every malloc/free path logs.
- `kernel/task.c` lines 170, 236, 276 — task create/exit logs.
- `kernel/ipc.c` — every error path logs.

The current `bench_quiet_flag` only gates `bench_info()` printing, **not** kernel `v_log()`. So the kernel is still printf-ing on every malloc during the malloc benchmark.

**Why this is the #1 fix:** the `malloc_8B` p50 is 16224 cycles. The actual first-fit walk over a near-empty heap should be ~80 cycles. The other 16000 are `v_log(LOG_DEBUG, ...)` formatting and DMA pushes. Same story for sem give/take, mutex lock, context switch.

**Fix:** introduce two levels of compile-out:
```c
/* vaios_config.h */
#ifndef VAIOS_KERNEL_LOG_LEVEL
#  define VAIOS_KERNEL_LOG_LEVEL LOG_WARN   /* default: errors + warnings only */
#endif

#define V_KLOG(level, ...) \
    do { if ((level) <= VAIOS_KERNEL_LOG_LEVEL) v_log((level), __VA_ARGS__); } while (0)
```

Replace every `v_log(LOG_DEBUG, ...)` in `memory.c`, `task.c`, `ipc.c` with `V_KLOG(LOG_DEBUG, ...)`. Default build drops all of them to zero. Flight build sets `VAIOS_KERNEL_LOG_LEVEL=LOG_ERROR` — only panics and corruption detection survive.

**Measurable impact (predicted):**
- `malloc_8B` p50: 16224 → ~400 cyc (40× improvement just from dropping logs)
- `sem_give_take` p50: 820 → ~250 cyc
- `ctx_switch_yield` p50: 1335 → ~500 cyc

**Estimated effort:** half a day, mechanical. Re-run the benchmark suite, confirm numbers, commit.

---

### 3.3 Allocator: O(n) → O(1) for free, segregated free list for malloc — **flight allocator must be deterministic**

**Where:**
- `kernel/memory.c:32-135` — `v_malloc` linear-scans from head every call.
- `kernel/memory.c:137-212` — `v_free` walks heap from head to find `prev` so it can coalesce.

For a 32 KB heap with 50 blocks, free is ~50 header dereferences ≈ 200 cycles even without logging. The combinatorial worst case under fragmentation is what kills determinism.

**Fix path (two stages):**

**Stage A — make `free` O(1):**
Add an explicit `prev` pointer in `Heap_Mem_Block`:
```c
typedef struct Heap_Mem_Block {
    uint32_t magic_number;
    uint32_t size;
    uint8_t  status;
    struct Heap_Mem_Block *prev;   /* NEW */
} Heap_Mem_Block;
```
`v_free` then computes `prev` directly (block->prev) and coalesces forward via `block + sizeof(hdr) + block->size`. No walk. Cost: 8 extra bytes per allocation, ~25% header overhead reduction.

**Stage B — segregated free lists for malloc:**
Keep an array of 8 free lists indexed by size class (8B, 16B, 32B, 64B, 128B, 256B, 512B, ≥1024B). On free, append to the appropriate class. On malloc, pop head of smallest class ≥ requested size. Splits go back into the class for the residual.

This is essentially FreeRTOS heap_5 / dlmalloc-lite. **The benchmark proves heap_4 (FreeRTOS) at p99 is 283 cyc vs vaios 29000 cyc — algorithm matters.**

**Measurable impact:**
- `malloc_8B` p99: 29000 → ~300 cyc (combined with §3.2)
- `malloc_fragmented` p99: 3867 → ~600 cyc
- `free_*` p99: 4000 → ~200 cyc

**Estimated effort:** 2 days for Stage A; 3 days for Stage B with unit tests.

---

### 3.4 Wait queues: FIFO → priority-ordered — **eliminates priority inversion at sema boundary**

**Where:** `kernel/ipc.c:10-33`. `wait_q_enqueue` appends to tail unconditionally.

**Why:** if a high-priority control task and a low-priority logger are both blocked on a shared sema, current code wakes them FIFO — so a slow logger that got there first delays the control task.

**Fix:** insertion-sort by priority on enqueue:
```c
static void wait_q_enqueue(sema_t *sem, TCB *task) {
    task->next = NULL;
    if (!sem->wait_q || sem->wait_q->priority < task->priority) {
        task->next = sem->wait_q;
        sem->wait_q = task;
        return;
    }
    TCB *p = sem->wait_q;
    while (p->next && p->next->priority >= task->priority) p = p->next;
    task->next = p->next;
    p->next = task;
}
```
O(n) on enqueue but with bounded n (≤ num tasks waiting on this sema, typically 1-3). On dequeue, head is highest-priority — O(1).

**Measurable impact:** create a new metric `sem_priority_order` (high-prio + low-prio both block on a sema; signal once; verify high-prio wakes first). Currently this would silently fail on vaios.

**Estimated effort:** half a day + new test.

---

### 3.5 Priority inheritance: one-shot → transitive + correct un-boost — **chained mutex correctness**

**Where:** `kernel/ipc.c:171-175` (one-shot boost) and `kernel/ipc.c:244-247` (incorrect un-boost).

**One-shot bug:**
```c
if (rm->owner && rm->owner->priority < current->priority) {
    task_change_priority(rm->owner, current->priority);
}
```
If the owner is itself blocked on another mutex M2 owned by T3, T3's priority is not boosted. T3 stays at its base priority, gets preempted by anything in between, the chain never resolves. This is **classic transitive inversion**.

**Un-boost bug:**
```c
if (current->priority > current->base_priority) {
    task_change_priority(current, current->base_priority);
}
```
If `current` still holds *another* mutex with high-priority waiters, this drops priority below what those waiters need. Correct behavior: priority = max(base_priority, max(priority of any waiter on any held mutex)).

**Fix:**
1. Track per-TCB list of held mutexes (`TCB *held_mutexes_head`, intrusive linked list via `mutex->next_held`).
2. `v_mutex_lock` chain-walks owner→wait_sem→owner-of-wait_sem boosts up to MAX_PI_DEPTH (suggest 4).
3. `v_mutex_unlock` recomputes priority as `max(base, max-waiter-prio-across-still-held-mutexes)`.

This is what Zephyr does and why its `mutex_pi_basic` is 6× faster than vaios — it's both faster AND correct under chains, vaios is slower AND incorrect.

**Measurable impact:**
- `mutex_pi_basic` cyc: 752623 → ~200000 (combined with §3.2 log strip and §3.4 prio queue).
- New metric `mutex_pi_chain_3deep` (currently in plan §4.2 as a "vaios will fail" entry) — should pass.

**Estimated effort:** 2 days, requires careful invariant testing.

---

### 3.6 Tick-driven wake → IRQ-driven preemption on delay expiry — **fixes `delay_accuracy` and `control_loop_jitter`**

**Where:** `kernel/task.c:341-356` (`wake_up_delayed_tasks`) and `task.c:181-214` (`get_next_task` calls it).

**The defect:** `wake_up_delayed_tasks` is only called from `get_next_task`, which only runs on PendSV (yield or preempt). A task that called `task_delay(5)` sleeps until *some other task* yields or the next SysTick fires. Worst case: delay completes mid-tick, but no wake-up happens until next tick → up to 1 ms late.

This is exactly what `control_loop_1khz_jitter` measures and why p50 = 983 µs.

**Fix:**

1. Move `wake_up_delayed_tasks()` into the `SysTick_Handler` body, not `get_next_task`.
2. When a wake happens and the woken task's priority > current task's priority, pend PendSV from SysTick:
```c
void SysTick_Handler(void) {
    v_tick_increment();
    bool woke_higher = wake_up_delayed_tasks_isr();
    if (woke_higher) v_port_trigger_pendsv();
}
```
3. Keep `delayed_list` sorted by `delay_ticks` ascending so SysTick only inspects the head — O(1) check, O(k) wake for k tasks due.

**Measurable impact:**
- `delay_accuracy_5ms` µs: 6000 → ~5050 (one-tick resolution limit).
- `control_loop_1khz_jitter` µs: 983 → < 50 µs at p99 (target).
- Adds maybe 30 cyc to every SysTick handler — acceptable.

**Estimated effort:** 1.5 days, includes sorted-list maintenance + edge cases for tick rollover.

---

### 3.7 Real ISR-to-task wake metric — **not strictly a vaios bug, but required to validate §3.1+§3.6**

**Where:** `BENCHMARK_PLAN.md` §4.1 specifies `isr_to_task_wake` but bench_latency.c only has the task-to-task proxy. The flight-critical metric is "hardware timer fires → IRQ runs → semaphore_give_from_isr → high-priority task's first user instruction".

**Fix:** add a `bench_isr_wake.c` to all three RTOSes using TIM2 in one-shot mode:
- Configure TIM2 to fire 1 µs from now.
- Record `bench_cyc()` just before arming.
- ISR records `bench_cyc()` and gives a binary sem.
- Woken task records `bench_cyc()` as first instruction, pushes delta.

This is the **single most important flight number.** If `isr_to_task_wake` p99 > 5 µs (420 cyc @ 84 MHz), the system can't service a 1 kHz IMU loop reliably under contention.

**Estimated effort:** 1 day across all three RTOSes (TIM2 init is the only RTOS-specific glue).

---

### 3.8 Flash footprint: 78 KB → < 40 KB — **important but secondary**

vaios's 78 KB is 3× FreeRTOS and 2× Zephyr largely because of:
- Built-in semihosting and terminal subsystems even when not used.
- Logging code paths kept even with `LOGGING_ENABLED=0` because of weak linkage.
- No `--gc-sections` (?? — verify).

Audit with `arm-none-eabi-nm --size-sort vaios.elf | tail -50` and `arm-none-eabi-objdump -h` to find largest symbols. The §3.2 log strip already removes a lot of strings; the rest is real code that needs feature-flagging.

**Estimated effort:** 1 day audit + iterative cleanup; treat as a follow-up to §3.2.

---

## 4. Sequencing & gates

The order matters because §3.2 (log strip) re-baselines every other measurement.

### Phase A — Cheap & high-impact (1 week)
1. §3.2 (log strip) — every measurement after this is the real number.
2. §3.1 (PRIMASK → BASEPRI) — flight correctness ship-blocker.
3. §3.6 (IRQ-driven wake) — fixes the 1 kHz loop usability.

**Gate A:** re-run full bench. `control_loop_1khz_jitter` p99 < 50 µs. `ctx_switch_yield` < 600 cyc. `malloc_8B` < 500 cyc.

### Phase B — Algorithmic (1.5 weeks)
4. §3.3 Stage A (O(1) free).
5. §3.4 (priority wait queues) + §3.5 (transitive PI).
6. §3.7 (real ISR-to-task metric).

**Gate B:** `mutex_pi_basic` < 200 k cyc. `mutex_pi_chain_3deep` passes. `isr_to_task_wake` p99 < 420 cyc (5 µs at 84 MHz).

### Phase C — Footprint & polish (3 days)
7. §3.3 Stage B (segregated free lists), if Stage A didn't close the gap.
8. §3.8 (flash audit).
9. Update `BENCHMARK_PLAN.md` §9 "Known traps" with what was learned.

**Gate C:** vaios within 2× of the better of FreeRTOS/Zephyr on every metric. No metric is worse than 3×. Full 24 h soak passes with `fail_count == 0`.

---

## 5. Things explicitly out of scope

- **Don't rewrite the scheduler from scratch.** The bitmap priority structure (`task.c:25-36`) is already O(1) for highest-prio lookup — that part is fine. The 8× ctx_switch gap is from logging + the FIFO append in `enqueue_task`; fix those, not the architecture.
- **Don't add MPU support.** Out of scope for parity with the other two RTOSes (Zephyr has `CONFIG_HW_STACK_PROTECTION=n` for the same reason).
- **Don't port FreeRTOS-style task notifications.** The semaphore API is enough for the flight workload.
- **Don't switch to tickless idle.** Power is not the flight constraint; deterministic latency is.

---

## 6. Risks

| Risk | Mitigation |
|---|---|
| BASEPRI change (§3.1) breaks NavHAL IRQ priority assumptions | Stage the change behind `#ifdef VAIOS_USE_BASEPRI`; run NavHAL sample suite before flipping the default. |
| Transitive PI (§3.5) introduces a deadlock-detection bug | Add an explicit MAX_PI_DEPTH cap that panics if exceeded; same as Zephyr's approach. |
| Segregated allocator (§3.3 Stage B) increases footprint | Stage A alone should close most of the gap; only do Stage B if measurements demand. |
| SysTick + wake_up + PendSV trigger (§3.6) inflates tick handler past 1 ms budget | Measure new `tick_jitter` immediately after change; should still be < 200 cyc total. |

---

## 7. Open questions

- Pick a target `BASEPRI` syscall ceiling: 5 (matches FreeRTOS default) or higher? Decide by enumerating which IRQs need to be unmaskable.
- Keep `recursion_count` on every mutex even though benchmarks show recursive mutexes aren't on the flight path? It's 4 bytes per mutex.
- Re-baseline vaios on Phase 1 (DWT timing) gate after §3.2: are the original numbers in `vaios/docs/benchmark/5_Mar_2026/` still meaningful or should they be replaced?
