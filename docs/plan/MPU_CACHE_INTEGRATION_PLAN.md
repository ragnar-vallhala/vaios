# vaios — NavHAL MPU + M7 Cache Integration Plan

Concrete, code-grounded plan to adopt NavHAL's ARMv7-M **MPU** (`hal_mpu`) and
**M7 cache** (`hal_cache`) drivers for two vaios goals: **hardware stack-overflow
detection** and **kernel/user memory separation**. Every edit cites a `file:line`
anchor verified against the tree at time of writing (branch `dev`, 2026-07-05).

> **Status: not started — blocked on Phase 0.** MPU/cache currently live only on
> NavHAL branch `feat/stm32f767zi-port` (driver commit `c5c8c19`), not yet on
> NavHAL `main`, which vaios `dev` tracks. Phase 0 cannot begin until the merge
> lands. See the branch model in the project memory (`vaios-branch-model`,
> `vaios-navhal-mpu-cache-plan`).

---

## NavHAL surface we build on

### `hal_mpu` (`extern/NavHAL/include/common/hal_mpu.h`)
- Runtime capability, never assumed: `hal_mpu_present()`, `hal_mpu_num_regions()`
  — **8 regions on F401 (M4), 16 on F767 (M7)**.
- Two-stage programming, purpose-built for context switches:
  - `hal_mpu_encode(idx, &region, &out)` — validate + pack RBAR/RASR **off the
    critical path**; cache the `hal_mpu_encoded_t` per task.
  - `hal_mpu_apply(set, count)` — write the pre-encoded pairs to hardware with a
    single DSB/ISB, no per-region validation. This is the PendSV fast path.
  - `hal_mpu_configure_region(idx, &region)` — validating one-shot for static setup.
- `hal_mpu_enable(bg_priv)` — `bg_priv` = `PRIVDEFENA`: privileged code keeps the
  default map for uncovered addresses; unprivileged always faults on uncovered.
- Descriptor (`hal_mpu_region_t`): `base` (**must be `size`-aligned**), `size`
  (pow2, 32 B..4 GB), `ap` (priv/unpriv split), `mem` preset (incl.
  `NORMAL_NONCACHE` for DMA, `DEVICE` for MMIO), `executable`, `shareable`,
  `srd_mask` (8-way subregion disable, needs `size >= 256 B`).
- Overlap rule: **highest-numbered enabled region wins**. Violation → MemManage.
- Gated by `NAVHAL_CONFIG_DRV_MPU` (mirror of `DRV_MPU` Kconfig). Absent MPU →
  every entry point returns `HAL_ERR_NOT_SUPPORTED`, `hal_mpu_present()` = false.

### `hal_cache` (`extern/NavHAL/include/common/hal_cache.h`, M7)
- `hal_icache_enable/disable/is_enabled`, `hal_dcache_enable/disable/is_enabled`.
- `hal_dcache_clean(addr,size)` (push to RAM, pre-DMA-TX),
  `hal_dcache_invalidate(addr,size)` (drop stale lines, post-DMA-RX),
  `hal_dcache_clean_invalidate(addr,size)`.

---

## Pre-work findings (informs sequencing)

- **Task stacks are not MPU-alignable as allocated.** `task_create_named` does
  `task->mem_block = (uint32_t *)v_malloc(stack_size)` (`kernel/task.c:197`) with
  only 4-byte alignment (`task.c:189`) and non-pow2 sizes. An MPU region needs a
  **`size`-aligned, power-of-two base** → Phase 1 must add an aligned allocator.
  The two-backend heap facade we just built (`kernel/memory/memory.c`) is the one
  place to add it.
- **Overflow detection today is reactive + software.** `kernel/task.c:301-308`
  compares `current_task->sp < mem_block + TASK_STACK_OVERFLOW_THRESHOLD` at
  switch time only, gated on `TASK_STACK_WATERMARK_ENABLE`. Late and coarse; MPU
  makes it a precise fault at the faulting store. Keep this path as the
  MPU-absent (QEMU/M4) fallback.
- **`PendSV_Handler` is naked asm but already calls C mid-handler.** `bl
  set_next_task` at `portable/cortex-m4/port.c:221` — a clean seam to apply the
  incoming task's encoded MPU set right after it returns, before the context
  restore at `port.c:224-245`.
- **No MemManage handler exists.** Only `HardFault_Handler` (`port.c:110`), which
  already decodes CFSR (`port.c:94`). MPU faults escalate to HardFault unless we
  set `SHCSR.MEMFAULTENA` and vector `MemManage_Handler`. Reuse the existing
  register-dump/`v_panic` path (`port.c:106`).
- **Tasks run privileged; no SVC syscall gateway.** `init_task_stack`
  (`port.c:286`) builds the exception frame but sets no `CONTROL.nPRIV`; SVC
  (`SVCall_Handler`, `port.c:252`) is used only for scheduler bootstrap, not
  kernel services. Kernel/user separation (Phase 3) needs both.
- **TCB has room for MPU state.** `TCB` (`include/task.h:34-63`) already carries
  `mem_block`/`stack_size`; add a small pre-encoded region array + count here.
- **Prior art in-tree.** `docs/changelog/2026-03-07-improve-memory-protection-and-panic-system.md`
  and the HardFault-diagnostics changelog show the panic/fault conventions to
  match.

---

## Phase 0 — Prerequisites (BLOCKING)

1. Confirm reachability after the NavHAL merge:
   `git -C extern/NavHAL merge-base --is-ancestor c5c8c19 origin/main`.
2. Bump `extern/NavHAL` submodule on `dev` (tracks NavHAL `main`); commit the pin.
3. Enable `CONFIG_DRV_MPU` (+ cache driver) in `navhal.config`; verify
   `NAVHAL_CONFIG_DRV_MPU` reaches the build via `navhal_target.h`.
4. Smoke test: `hal_mpu_present()` / `hal_mpu_num_regions()` → 8 on F401, 16 on
   F767. Nothing else changes yet.

**Verify:** existing suites green on both targets + QEMU with MPU driver linked
but unused.

---

## Phase 1 — MPU stack-overflow guard  *(highest ROI, lowest risk — do first)*

### 1.1 Aligned stack allocator
New `v_memalign(size_t align, size_t size)` in the heap facade
(`kernel/memory/memory.c`), delegating to the active backend. Stacks allocated
size-aligned so a guard region has a legal base. Guard `#if VAIOS_MPU_ENABLE`;
otherwise `task_create` keeps `v_malloc`.

### 1.2 Guard region at stack base
Reserve a 32 B `HAL_MPU_AP_NONE` region at the low end of each task stack (just
below `mem_block`, `task.c:197`). Any push past the stack bottom writes into it →
MemManage. `hal_mpu_encode()` it once in `task_create_named` (`task.c:185`),
store the `hal_mpu_encoded_t` + count in new TCB fields (`include/task.h:34`).

### 1.3 Apply on context switch
Add a tiny C hook (e.g. `v_port_mpu_apply_current()`) called from
`PendSV_Handler` immediately after `bl set_next_task` (`port.c:221`); it invokes
`hal_mpu_apply(current_task->mpu_set, current_task->mpu_count)`. Keep it in C to
avoid growing the naked asm. No-op when `!hal_mpu_present()`.

### 1.4 MemManage handler
Set `SHCSR.MEMFAULTENA` in init; add `MemManage_Handler` decoding `CFSR.MMFSR` +
`MMFAR`, mapping the fault address back to the offending task, then `v_panic`
("stack overflow in task N") reusing the message at `task.c:304` and the dump
path at `port.c:94-107`.

### 1.5 Fallback + config
Gate the software watermark (`task.c:301`) vs. the MPU guard on
`hal_mpu_present()` so QEMU/M4-without-MPU keep the old behavior. New knobs in
`include/vaios_config_default.h` (near `HEAP_WATERMARK_THRESHOLD`, ~line 139):
`VAIOS_MPU_ENABLE`, `VAIOS_MPU_STACK_GUARD`.

**Verify:** new `MEM_PROTECT` example (deliberate overflow) faults precisely on
Renode + real F401/F767; watermark path unchanged under QEMU.

---

## Phase 2 — Static kernel / peripheral protection

Program a background region map within the 8/16 budget, then
`hal_mpu_enable(/*bg_priv=*/true)`:

| Region intent      | AP                         | mem preset            | XN |
|--------------------|----------------------------|-----------------------|----|
| Flash (code)       | `HAL_MPU_AP_RO`            | `NORMAL_WT`           | no |
| SRAM (data)        | `HAL_MPU_AP_RW`           | `NORMAL_WB`           | yes|
| Kernel `.data/.bss`| `HAL_MPU_AP_PRIV_RW`      | `NORMAL_WB`           | yes|
| MMIO / peripherals | `HAL_MPU_AP_PRIV_RW`      | `DEVICE`              | yes|

Catches NULL-deref, execute-from-RAM, and wild peripheral access independent of
the task model. Needs linker symbols for the kernel data span
(`extern/NavHAL` / vaios linker scripts) to bound the privileged region. Highest
region number reserved for Phase 1's per-task guard so it wins on overlap.

**Verify:** deliberate NULL write / RAM-execute faults; normal workloads
unaffected.

---

## Phase 3 — Kernel/user privilege separation  *(largest; own PR series; ABI change)*

### 3.1 Unprivileged tasks
Set `CONTROL.nPRIV` for task execution — established via the exception-return
frame in `init_task_stack` (`port.c:286`) and re-asserted per switch. Kernel code
(handlers, scheduler) stays privileged.

### 3.2 Per-task region set
Extend the Phase 1 TCB set from {guard} to a full layered map: own stack
RW-unpriv, code RX, everything else no-access — layered over Phase 2 background
(highest-region-wins). Encode at task setup, apply in PendSV (reuses 1.3).

### 3.3 SVC syscall gateway
Route kernel services (`v_malloc`/`v_free`, IPC in `kernel/ipc.c`, task ops)
through SVC so unprivileged tasks cannot touch kernel memory directly. Extend
`kernel/syscalls.c` + `SVCall_Handler` (`port.c:252`) with a syscall number ABI
and argument marshalling. **This is the ABI change** — public task-facing calls
become SVC stubs.

### 3.4 Region budget
On M4 (8 regions) background + per-task set must fit; use `srd_mask` subregion
disables to economize. Assert `count <= hal_mpu_num_regions()` at setup.

**Verify:** an unprivileged task faults on kernel-memory access and on direct
peripheral poke; the same operation via syscall succeeds.

---

## Phase 4 — M7 cache (F767 only)

1. Enable **I-cache then D-cache** at init, *after* MPU is live (order matters):
   `hal_icache_enable()` → `hal_dcache_enable()`.
2. DMA coherency — pick per buffer:
   - Place DMA pools in a `NORMAL_NONCACHE` MPU region (simplest, slight perf cost), or
   - Wrap transfers: `hal_dcache_clean(buf,len)` before TX,
     `hal_dcache_invalidate(buf,len)` after RX.
3. Add a `.dma`/non-cacheable linker section for shared buffers.

All `#if` gated on M7 / `hal_dcache` presence; M4 untouched.

**Verify:** DMA round-trip integrity with D-cache on (F767); measure the
allocator/ctx-switch benchmarks with caches on vs. off.

---

## Cross-cutting

- **Portability:** every hook behind `hal_mpu_present()` / `NAVHAL`; QEMU, M4, and
  non-NavHAL builds degrade to the software watermark and skip cache.
- **Testing:** QEMU's MPU model is partial — validate faults on **Renode** and
  real F401/F767 (see project memory `qemu-cannot-run-navhal-suite`). Add a
  `MEM_PROTECT` example next to the new `MEM_STRESS`/`MEM_FRAG`
  (`examples/CMakeLists.txt`).
- **Sequencing:** Phases 1, 2, 4 are largely independent and low-risk; Phase 3 is
  the heavy, ABI-affecting one and should ship as its own PR series after 1–2.
- **Config knobs (all default off / capability-gated):** `VAIOS_MPU_ENABLE`,
  `VAIOS_MPU_STACK_GUARD`, `VAIOS_MPU_KERNEL_SEPARATION`, `VAIOS_CACHE_ENABLE`.

---

## Risks

- **Alignment cost:** size-aligned pow2 stacks waste RAM vs. today's tight
  `v_malloc`. Quantify on the F401 (8 regions, less RAM) before committing Phase 1
  defaults.
- **Region scarcity on M4:** 8 regions must cover background + per-task; Phase 3
  may be F767-first if it doesn't fit.
- **SVC ABI churn:** Phase 3 changes how tasks call the kernel — coordinate with
  any external users of the task API before landing.
- **Cache/MPU attribute mismatch:** wrong TEX/C/B/S vs. cache state is a classic
  silent-corruption source; lean on `NORMAL_NONCACHE` for DMA until validated.
