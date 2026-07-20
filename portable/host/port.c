#define _GNU_SOURCE // ucontext_t, sigprocmask, sched_yield on glibc
/**
 * @file port.c
 * @brief Host (POSIX) port — context switch, critical sections, task bring-up.
 *
 * The scheduler runs for real here: each task gets a ucontext, swapcontext IS
 * the context switch (the PendSV analogue), and a SIGALRM timer (armed in
 * port_hw.c) drives preemption. The kernel is reused verbatim; the two seams it
 * switches through — v_port_trigger_pendsv() and set_next_task() — are all this
 * layer needs to hook. See docs/plan/HOST_PORT_PLAN.md.
 *
 * Signal-mask model (the subtle part): SIGALRM is the "interrupt". Critical
 * sections block it (== BASEPRI masking SysTick). A context switch runs with
 * SIGALRM blocked and is performed only at a safe point — the outermost
 * EXIT_CRITICAL, or the tick handler's exit — never mid-critical-section and
 * never mid-switch. A freshly launched task explicitly unblocks SIGALRM so it is
 * preemptible from its first instruction.
 */

#include "port.h"
#include "task.h"  // TCB, current_task, set_next_task, v_task_exit_impl
#include "utils.h" // v_kernel_tick
#include <sched.h> // sched_yield
#include <signal.h>
#include <stddef.h>
#include <stdlib.h> // malloc / free
#include <ucontext.h>
#include <unistd.h>

// Kernel globals we drive.
extern TCB *current_task;
extern uint8_t scheduler_running;

// Backing storage for the kernel heap. On target this is a linker-script region
// (&_heap_start); on host it is a plain static arena of the configured size.
// memory.c declares `extern uint32_t _heap_start;` and uses `&_heap_start`.
uint32_t _heap_start[HEAP_SIZE / sizeof(uint32_t)];

// --- Critical sections =======================================================
volatile uint32_t critical_nesting = 0;

// {SIGALRM}, filled once at program start so critical sections used during early
// init (before the timer is armed) are already safe.
static sigset_t g_alrm;
__attribute__((constructor)) static void host_port_ctor(void) {
  sigemptyset(&g_alrm);
  sigaddset(&g_alrm, SIGALRM);
}

static void host_maybe_switch(void);

void v_enter_critical(void) {
  sigprocmask(SIG_BLOCK, &g_alrm, NULL); // mask the tick (== raise BASEPRI)
  critical_nesting++;
}

void v_exit_critical(void) {
  if (critical_nesting > 0)
    critical_nesting--;
  if (critical_nesting == 0) {
    sigprocmask(SIG_UNBLOCK, &g_alrm, NULL); // may deliver a tick pended while masked
    host_maybe_switch();                     // perform a cooperatively-pended switch
  }
}

uint32_t v_enter_critical_from_isr(void) {
  sigset_t old;
  sigprocmask(SIG_BLOCK, &g_alrm, &old);
  return sigismember(&old, SIGALRM) ? 1u : 0u; // "was already masked"
}

void v_exit_critical_from_isr(uint32_t saved) {
  if (!saved)
    sigprocmask(SIG_UNBLOCK, &g_alrm, NULL);
}

// --- Context switch engine ===================================================
static volatile sig_atomic_t g_switch_pending = 0;
static volatile sig_atomic_t g_in_isr = 0;
static ucontext_t g_bootstrap; // scheduler_start's context; returned to on halt

static inline ucontext_t *ctx_of(TCB *t) { return (ucontext_t *)t->sp; }

// Save the running task, pick the next, load it. Runs with SIGALRM blocked so a
// tick can't fire mid-switch; swapcontext saves the outgoing task's mask (blocked
// here) and restores the incoming task's, and the matching UNBLOCK below — reached
// when this task is later resumed — re-arms preemption.
static void host_perform_switch(void) {
  sigprocmask(SIG_BLOCK, &g_alrm, NULL);
  TCB *prev = current_task;
  set_next_task(); // updates current_task to the next runnable task
  if (current_task != prev)
    swapcontext(ctx_of(prev), ctx_of(current_task));
  sigprocmask(SIG_UNBLOCK, &g_alrm, NULL);
}

// The PendSV analogue: switch only if one is pending and we are at a safe point
// (not inside the tick handler, not inside a critical section).
static void host_maybe_switch(void) {
  if (!g_switch_pending || g_in_isr || critical_nesting != 0)
    return;
  g_switch_pending = 0;
  host_perform_switch();
}

// Requested from v_kernel_tick (preemption) and kernel blocking paths. Deferred,
// exactly like pending PendSV: the switch happens at the next safe point.
void v_port_trigger_pendsv(void) { g_switch_pending = 1; }

// Called from the SIGALRM handler (port_hw.c) — the SysTick ISR analogue. Do the
// tick work "in ISR", then perform any pended switch on the way out (PendSV
// tail-chaining after SysTick).
void v_host_on_tick(void) {
  g_in_isr = 1;
  v_kernel_tick();
  g_in_isr = 0;
  host_maybe_switch();
}

int v_port_hw_in_isr(void) { return g_in_isr; }

// --- Task bring-up ===========================================================
static void task_trampoline(void) {
  // A newly launched task must be preemptible from its first instruction,
  // regardless of the signal mask in effect when its context was created.
  sigprocmask(SIG_UNBLOCK, &g_alrm, NULL);

  current_task->entry(current_task->arg);

  // The task function returned: terminate it and switch away for good. This
  // ucontext's stack is abandoned; control never comes back here.
  v_task_exit_impl();    // mark TERMINATED + pend a reschedule
  g_switch_pending = 1;
  host_perform_switch(); // load the next task; the dead stack is never resumed
  __builtin_unreachable();
}

// A ucontext execution stack must clear MINSIGSTKSZ — kilobytes. Rather than
// force callers to size task->mem_block that big (they'd have to write host-only
// stack sizes), the port allocates the ucontext + its stack SEPARATELY, sized to
// a host-appropriate default, so a task created with a normal on-target stack
// size (>= VAIOS_ARCH_MIN_STACK, 128 B) just works. A caller that asks for more
// than the default gets it.
#define HOST_STACK_MIN (64u * 1024u)

// init_task_stack builds the ucontext instead of a fake exception frame. task->sp
// (never a raw SP on host) carries the port allocation so the switch engine and
// the teardown hook can find it; task->mem_block is left to the kernel.
void init_task_stack(TCB *task) {
  size_t stacksz = task->stack_size;
  if (stacksz < HOST_STACK_MIN)
    stacksz = HOST_STACK_MIN;
  // malloc is not reentrant; mask the tick so a preemption into another
  // allocating task can't corrupt the arena.
  v_enter_critical();
  char *blk = (char *)malloc(sizeof(ucontext_t) + stacksz);
  v_exit_critical();
  ucontext_t *uc = (ucontext_t *)blk;
  getcontext(uc);
  uc->uc_stack.ss_sp = blk + sizeof(ucontext_t); // makecontext aligns internally
  uc->uc_stack.ss_size = stacksz;
  uc->uc_link = NULL; // trampoline never returns
  makecontext(uc, task_trampoline, 0);
  task->sp = (uint32_t *)uc;
}

// Free the ucontext allocation of a terminated task (called by the dead-task GC).
// The task is dead — swapped away for good — so its frozen context is safe to
// release.
void v_port_free_task_stack(TCB *task) {
  if (task->sp) {
    v_enter_critical();
    free(task->sp);
    v_exit_critical();
    task->sp = NULL;
  }
}

// scheduler_start: launch the current task (the idle task, seeded by
// scheduler_init). The first tick then preempts to the highest-ready task, just
// as the first SysTick does on target.
void scheduler_start(void) {
  scheduler_running = 1;
  sigprocmask(SIG_BLOCK, &g_alrm, NULL);
  swapcontext(&g_bootstrap, ctx_of(current_task));
  // Reached only if a task swaps back to the bootstrap (system halt).
  sigprocmask(SIG_UNBLOCK, &g_alrm, NULL);
}

// --- Small facade pieces =====================================================
uint32_t v_port_get_psp(void) { return 0; } // no PSP -> SP-based guards skip
int v_port_ptr_is_ram(const void *p) {
  (void)p;
  return 1; // no known target map on host
}
void v_port_disable_interrupts(void) { sigprocmask(SIG_BLOCK, &g_alrm, NULL); }
void v_port_halt(void) {
  sigprocmask(SIG_BLOCK, &g_alrm, NULL);
  for (;;)
    pause();
}
void v_port_cpu_relax(void) { sched_yield(); }

// MPU: none on host.
void v_port_mpu_init(void) {}
int v_port_stack_guard_encode(void *base, uint32_t size, uint32_t out[2]) {
  (void)base;
  (void)size;
  (void)out;
  return -1; // unavailable -> kernel marks the guard invalid and skips it
}
#if VAIOS_MPU_USER_SEPARATION
int v_port_task_region_encode(void *base, uint32_t size, uint32_t out[2]) {
  (void)base;
  (void)size;
  (void)out;
  return -1;
}
#endif
void v_port_mpu_apply(const uint32_t enc[2], uint32_t count) {
  (void)enc;
  (void)count;
}
