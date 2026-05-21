# vaios Flight-Profile Phased Implementation Plan

Concrete, code-grounded execution plan for `../VAIOS_FLIGHT_IMPROVEMENT_PLAN.md`. Every edit cites a file:line anchor verified against the tree at the time of writing.

## Pre-work findings (informs sequencing)

- `MAX_SYSCALL_INTERRUPT_PRIORITY` is already defined in `include/vaios_config_default.h:55` and already used by `PendSV_Handler` / `load_next_task_from_isr` in `portable/cortex-m4/port.c:246,314`. Only `v_enter_critical` / `v_exit_critical` at `port.c:10-21` still use `cpsid i`. §3.1 is a tighter change than the source doc implies.
- `Heap_Mem_Block` (`include/memory.h:10`) already has a `uint32_t padding` slot, so §3.3 Stage A's `prev*` can repurpose that field — no header-size delta, no allocator overhead change.
- `SysTick_Handler` (`kernel/utils.c:911`) is trivially small; moving wake into it is mechanical.
- `wake_up_delayed_tasks` (`kernel/task.c:318`) scans the entire `delayed_list` every call from `get_next_task`. The §3.6 sorted-by-deadline rework is also what makes the SysTick fast path cheap.
- `idle_task_function` calls `v_log_flush()` 64× per idle iteration (`task.c:257-258`). Verify this is a no-op under `LOGGING_ENABLED=0` once §3.2 lands.

---

## Phase A — Cheap & high-impact (target: 1 week)

### A1. §3.2 Log strip — DO FIRST (re-baselines everything else)

New macro in `include/utils.h` next to the `Log_Type` enum:
```c
#ifndef VAIOS_KERNEL_LOG_LEVEL
#  define VAIOS_KERNEL_LOG_LEVEL LOG_WARN
#endif
#define V_KLOG(level, ...) \
    do { if ((int)(level) >= (int)VAIOS_KERNEL_LOG_LEVEL) v_log((level), __VA_ARGS__); } while (0)
```

Mechanical replace of `v_log(LOG_DEBUG, ...)` with `V_KLOG(LOG_DEBUG, ...)` in:
- `kernel/memory.c` lines 23, 46, 70, 85, 109, 133, 139, 155, 161, 178, 185, 211
- `kernel/task.c` lines 170, 236, 276
- `kernel/ipc.c` — repeat on every error/debug path (`grep -n v_log kernel/ipc.c`)

`LOG_INFO` / `LOG_ERROR` stay as-is — those don't run on the hot path.

Default in `vaios_config_default.h`: `VAIOS_KERNEL_LOG_LEVEL = LOG_WARN`. Flight build raises to `LOG_ERROR`.

**Verify**: rebuild, re-run benchmark suite, commit numbers under `results/<date>-post-log-strip/`. Predicted: `malloc_8B` p50 ~400 cyc, `ctx_switch_yield` ~500 cyc.

---

### A2. §3.1 BASEPRI critical sections

Edit `portable/cortex-m4/port.c:10-21`:
```c
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
- Keep `v_port_disable_interrupts` at `port.c:29` using `cpsid i` — it's used by `v_panic` (`utils.c:932`) and should still mask everything.
- PendSV/SVC handlers at `port.c:215,268` already do `msr basepri` correctly — no change.

Gate behind `#ifdef VAIOS_USE_BASEPRI` per source-doc Risk #1 until NavHAL is re-validated; default on after smoke test.

Document NVIC priority bands in `vaios_config_default.h`; provide a `port_init_nvic()` stub the boot path can call. No NavHAL changes required for the kernel-only commit.

Re-introduce the `kernel_critical_section_max` benchmark (currently SKIPped on vaios) to prove the regression in masking duration is finite.

---

### A3. §3.6 IRQ-driven wake (depends on A1, not A2)

Three pieces in `kernel/task.c`:

1. Convert `delayed_list` to sorted insertion at `add_to_delayed_list` (`task.c:290`). Insertion is O(n) on the list; in flight workloads the list is short and stays O(1) at the head.
2. Split `wake_up_delayed_tasks` (`task.c:318`):
   - New `wake_up_delayed_tasks_isr()` — inspects only the head; loops while `head->delay_ticks <= now`; returns `true` if any woken task has priority > current.
   - Keep the second loop (timeout-eject on `blocked_list` semaphore waiters) — fold into the same SysTick call, or leave on the PendSV path; pick by measurement.
3. Move the wake call out of `get_next_task` (`task.c:183`) into `SysTick_Handler` (`utils.c:911`):
   ```c
   void SysTick_Handler(void) {
       systick_count++;
       systick_ticks++;
       if (scheduler_running) {
           if (wake_up_delayed_tasks_isr()) v_port_trigger_pendsv();
           else if (/* time-slice expired */) v_port_trigger_pendsv();
       }
   }
   ```
   Today PendSV is pended unconditionally every tick — keep that as the fallback. The woken-higher branch is what closes the 983 µs jitter gap.

**Gate A**: re-run bench. `control_loop_1khz_jitter` p99 < 50 µs, `ctx_switch_yield` < 600 cyc, `malloc_8B` < 500 cyc. If any miss, stop and diagnose before Phase B.

---

## Phase B — Algorithmic (target: 1.5 weeks)

### B1. §3.3 Stage A — O(1) free

In `include/memory.h:10`, replace `padding` with `struct Heap_Mem_Block *prev`. Size stays 16 bytes.

In `kernel/memory.c`:
- `v_malloc` split path (line 91) — set `newBlock->prev = head;` and walk forward to the next block to fix up its `prev`.
- `v_malloc` fresh-block path (line 62) — track `last_fresh_block` as a file-static so the next fresh allocation gets a correct `prev`.
- `v_free` (line 137) — delete the head-walk at lines 172-191; coalesce backward via `block->prev`, forward via the existing trailer-address compute. The corruption check loop becomes O(1).

Existing heap unit tests must still pass. Add a test that frees in reverse order and asserts a single coalesced free block.

---

### B2. §3.4 Priority-ordered wait queues (prerequisite for B3)

`kernel/ipc.c:10-33` — replace `wait_q_enqueue` with insertion-sort by priority. Dequeue stays O(1). The timeout-eject loop at `task.c:336-368` still walks linearly — correct, just note in the commit.

Add `bench_sem_priority_order`: high-prio + low-prio both block on a binary sema; give once; assert high-prio runs first.

---

### B3. §3.5 Transitive PI + correct un-boost

Schema:
- `TCB` (`include/task.h`): add `rmutex_t *held_mutexes_head;`. `base_priority` already exists (`task.c:159`).
- `rmutex_t` (`include/ipc.h`): add `struct rmutex_t *next_held;` for the intrusive list.

Rework in `kernel/ipc.c`:
- `v_mutex_lock` (line 164): replace one-shot boost at lines 172-175 with chain walk — while `owner` is itself blocked on a mutex whose owner has lower priority, boost; cap at `MAX_PI_DEPTH=4` and panic if exceeded.
- `v_mutex_lock` on `VA_PASS`: link the new mutex onto `current->held_mutexes_head`.
- `v_mutex_unlock` (line 232): unlink from `held_mutexes_head`; recompute `current->priority = max(base_priority, max over remaining held mutexes of max waiter priority)`. The unconditional drop-to-base at lines 245-247 is wrong and must go.

Add `bench_mutex_pi_chain_3deep`: three tasks, two nested mutexes, assert mid-prio does NOT preempt low-prio while high-prio is blocked.

---

### B4. §3.7 Real ISR-to-task wake benchmark

Not a vaios change. Add `bench_isr_wake.c` mirrored across vaios / FreeRTOS / Zephyr using TIM2 one-shot. Only blocker: needs A2 (BASEPRI), since today PWM/IMU IRQs could be masked at the moment of measurement.

**Gate B**: `mutex_pi_basic` < 200k cyc, `mutex_pi_chain_3deep` passes, `isr_to_task_wake` p99 < 420 cyc (5 µs at 84 MHz).

---

## Phase C — Footprint & polish (target: 3 days)

- §3.3 Stage B (segregated free lists) — only if Stage A doesn't get malloc within 2× of FreeRTOS.
- §3.8 Flash audit — `arm-none-eabi-nm --size-sort vaios.elf | tail -50`; verify `--gc-sections` in `CMakeLists.txt`; feature-flag semihosting/terminal.
- Update `BENCHMARK_PLAN.md` §9 ("Known traps") with lessons learned.

**Gate C**: every metric within 2× of the better of FreeRTOS/Zephyr; 24h soak with `fail_count == 0`.

---

## Open questions to settle before coding

1. The source doc's `MAX_SYSCALL_INTERRUPT_PRIORITY` example is `5 << 4`; current config is `7 << (8 - __NVIC_PRIO_BITS)`. Keep current value or change to 5? Affects how many priority levels remain unmaskable.
2. `idle_task_function` runs `v_log_flush()` 64× per iter — confirm no-op under `LOGGING_ENABLED=0`, otherwise A1 doesn't fully land for the idle path.
3. `get_task_by_id` (`task.c:461`) is O(n) under a critical section, called from `task_unblock` / `task_exit_request`. Not on the 1 kHz path. Flag as latent; revisit in Phase B if soak surfaces it.

---

## Suggested first commit

A1 alone, so the re-baseline numbers are honest before any algorithmic change lands. A2 and A3 follow as separate commits in the same week.
