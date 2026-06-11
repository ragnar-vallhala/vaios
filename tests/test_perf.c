/**
 * @file test_perf.c
 * @brief Unit tests for kernel/perf.c — the optional perf module.
 *
 * Covers every public counter (sched, isr, ipc, heap), the composite
 * snapshot, v_perf_reset semantics, and v_perf_dump_to_file's behaviour
 * against the recorded-call VFS stub. Does NOT depend on a running
 * scheduler — each test drives the hooks directly.
 */
#include "framework.h"
#include "perf.h"
#include "perf_hooks.h"
#include "task.h"
#include "v_fs_stub.h"
#include "vfs.h" /* VFS_O_* flag macros */

extern TCB *current_task;
extern TCB *idle_task;
extern uint32_t allocation_size; /* kernel/memory.c — written by tests */

/* ---------------------------------------------------------------------- */

static TCB make_tcb(uint32_t id, uint32_t prio) {
  TCB t = {0};
  t.task_id = id;
  t.priority = prio;
  t.magic = TCB_MAGIC;
  return t;
}

/* Zero state between tests. Don't leave current_task pointing at a stack
 * TCB from a previous test — v_perf_reset re-primes its last_scheduled_cyc
 * which would dereference freed stack memory. */
static void reset(void) {
  current_task = NULL;
  idle_task = NULL;
  allocation_size = 0;
  v_perf_reset();
}

/* Some tests need a valid current_task because they call into vfs_open,
 * which on the host build routes through v_mutex_lock — that derefs
 * current_task. The earlier suites (run_vfs_tests) leave vfs_mutex
 * non-NULL, so we can't bypass the lock. Use this sentinel TCB. */
static TCB _vfs_dummy_task = {.task_id = 99, .priority = 1, .magic = TCB_MAGIC};
static void reset_with_current_task(void) {
  reset();
  current_task = &_vfs_dummy_task;
}

/* ---------------------------------------------------------------------- */
/* Scheduler counters                                                     */
/* ---------------------------------------------------------------------- */

static void test_reset_zeros_sched_switches(void) {
  reset();
  v_perf_on_sched_switch(NULL, NULL);
  v_perf_on_sched_switch(NULL, NULL);
  TEST_ASSERT(v_perf_sched_switches() >= 2u);
  v_perf_reset();
  TEST_ASSERT_EQ(v_perf_sched_switches(), 0u);
  TEST_ASSERT_EQ(v_perf_idle_cycles(), 0ULL);
}

static void test_sched_switch_counter_monotonic(void) {
  reset();
  v_perf_on_sched_switch(NULL, NULL);
  v_perf_on_sched_switch(NULL, NULL);
  v_perf_on_sched_switch(NULL, NULL);
  TEST_ASSERT_EQ(v_perf_sched_switches(), 3u);
}

static void test_sched_per_task_accumulate(void) {
  reset();
  TCB a = make_tcb(1, 1);
  TCB b = make_tcb(2, 2);
  v_perf_on_sched_switch(NULL, &a);   /* a runs */
  v_perf_on_sched_switch(&a, &b);     /* a→b: a credited */
  v_perf_on_sched_switch(&b, &a);     /* b→a: b credited, a runs again */
  TEST_ASSERT(a.perf.cycles_run > 0);
  TEST_ASSERT(b.perf.cycles_run > 0);
  TEST_ASSERT_EQ(a.perf.switches_in, 2u);
  TEST_ASSERT_EQ(b.perf.switches_in, 1u);
}

static void test_sched_idle_accounting(void) {
  reset();
  TCB i = make_tcb(0, 0);
  TCB t = make_tcb(1, 1);
  idle_task = &i;
  v_perf_on_sched_switch(NULL, &i);
  v_perf_on_sched_switch(&i, &t);     /* idle leaves → credit idle_cycles */
  TEST_ASSERT(v_perf_idle_cycles() > 0ULL);

  /* Switching between non-idle tasks must NOT bump idle_cycles. */
  uint64_t after_idle = v_perf_idle_cycles();
  TCB u = make_tcb(2, 2);
  v_perf_on_sched_switch(&t, &u);
  TEST_ASSERT_EQ(v_perf_idle_cycles(), after_idle);
}

static void test_sched_max_burst_tracks_largest(void) {
  reset();
  TCB t = make_tcb(1, 1);
  /* Two switches: each computes a delta. The host cycle stub strides by
   * 100 per read, so each successive delta should be roughly constant —
   * but max_burst_cyc must at least match cycles_run / switches_in. */
  v_perf_on_sched_switch(NULL, &t);
  v_perf_on_sched_switch(&t, NULL);
  TEST_ASSERT(t.perf.max_burst_cyc > 0);
  TEST_ASSERT(t.perf.cycles_run >= t.perf.max_burst_cyc);
}

/* ---------------------------------------------------------------------- */
/* ISR / SysTick                                                          */
/* ---------------------------------------------------------------------- */

static void test_isr_systick_min_max_last(void) {
  reset();
  v_perf_on_isr_systick_exit(100, 0);
  v_perf_on_isr_systick_exit(50, 0);
  v_perf_on_isr_systick_exit(200, 0);
  v_perf_isr_t out;
  v_perf_isr_stats(&out);
  TEST_ASSERT_EQ(out.systick_count, 3u);
  TEST_ASSERT_EQ(out.systick_min_cyc, 50u);
  TEST_ASSERT_EQ(out.systick_max_cyc, 200u);
  TEST_ASSERT_EQ(out.systick_last_cyc, 200u);
}

static void test_isr_systick_preemption_counter(void) {
  reset();
  v_perf_on_isr_systick_exit(100, 0);
  v_perf_on_isr_systick_exit(100, 1);
  v_perf_on_isr_systick_exit(100, 1);
  v_perf_on_isr_systick_exit(100, 0);
  v_perf_isr_t out;
  v_perf_isr_stats(&out);
  TEST_ASSERT_EQ(out.systick_count, 4u);
  TEST_ASSERT_EQ(out.systick_preemptions, 2u);
}

/* ---------------------------------------------------------------------- */
/* IPC                                                                    */
/* ---------------------------------------------------------------------- */

static void test_ipc_counters_independent(void) {
  reset();
  v_perf_on_ipc_take_attempt();
  v_perf_on_ipc_take_attempt();
  v_perf_on_ipc_take_blocked();
  v_perf_on_ipc_give();
  v_perf_on_ipc_give();
  v_perf_on_ipc_give();
  v_perf_on_ipc_timeout();
  v_perf_ipc_t out;
  v_perf_ipc_stats(&out);
  TEST_ASSERT_EQ(out.takes, 2u);
  TEST_ASSERT_EQ(out.takes_blocked, 1u);
  TEST_ASSERT_EQ(out.gives, 3u);
  TEST_ASSERT_EQ(out.timeouts, 1u);
}

/* ---------------------------------------------------------------------- */
/* Heap                                                                   */
/* ---------------------------------------------------------------------- */

static void test_heap_alloc_per_class_bucketing(void) {
  reset();
  v_perf_on_heap_alloc(8, 0);     /* class 0 (<=8)   */
  v_perf_on_heap_alloc(9, 0);     /* class 1 (<=16)  */
  v_perf_on_heap_alloc(32, 1);    /* class 2 (<=32), split */
  v_perf_on_heap_alloc(1024, 0);  /* class 7 (>512)  */
  v_perf_heap_t out;
  v_perf_heap_stats(&out);
  TEST_ASSERT_EQ(out.allocs, 4u);
  TEST_ASSERT_EQ(out.splits, 1u);
  TEST_ASSERT_EQ(out.per_class_allocs[0], 1u);
  TEST_ASSERT_EQ(out.per_class_allocs[1], 1u);
  TEST_ASSERT_EQ(out.per_class_allocs[2], 1u);
  TEST_ASSERT_EQ(out.per_class_allocs[7], 1u);
}

static void test_heap_free_coalesces_sum(void) {
  reset();
  v_perf_on_heap_free(0);
  v_perf_on_heap_free(1);
  v_perf_on_heap_free(2);
  v_perf_heap_t out;
  v_perf_heap_stats(&out);
  TEST_ASSERT_EQ(out.frees, 3u);
  TEST_ASSERT_EQ(out.coalesces, 3u);
}

static void test_heap_oom_separate_from_allocs(void) {
  reset();
  v_perf_on_heap_alloc(8, 0);
  v_perf_on_heap_oom();
  v_perf_on_heap_oom();
  v_perf_heap_t out;
  v_perf_heap_stats(&out);
  TEST_ASSERT_EQ(out.allocs, 1u);
  TEST_ASSERT_EQ(out.oom, 2u);
}

static void test_heap_peak_is_monotonic(void) {
  reset();
  allocation_size = 1000;
  v_perf_on_heap_alloc(8, 0);
  allocation_size = 500;
  v_perf_on_heap_alloc(8, 0);     /* peak must stay at 1000 */
  allocation_size = 2000;
  v_perf_on_heap_alloc(8, 0);     /* peak now 2000          */
  allocation_size = 100;
  v_perf_on_heap_alloc(8, 0);     /* peak still 2000        */
  v_perf_heap_t out;
  v_perf_heap_stats(&out);
  TEST_ASSERT_EQ(out.peak_bytes_in_use, 2000u);
}

/* ---------------------------------------------------------------------- */
/* Composite snapshot                                                     */
/* ---------------------------------------------------------------------- */

static void test_snapshot_bundles_all_subsystems(void) {
  reset();
  v_perf_on_sched_switch(NULL, NULL);
  v_perf_on_isr_systick_exit(123, 1);
  v_perf_on_ipc_take_attempt();
  v_perf_on_heap_alloc(64, 0);
  v_perf_snapshot_t s = {0};
  v_perf_snapshot(&s);
  TEST_ASSERT(s.sched_switches >= 1u);
  TEST_ASSERT_EQ(s.isr.systick_count, 1u);
  TEST_ASSERT_EQ(s.isr.systick_last_cyc, 123u);
  TEST_ASSERT_EQ(s.isr.systick_preemptions, 1u);
  TEST_ASSERT_EQ(s.ipc.takes, 1u);
  TEST_ASSERT_EQ(s.heap.allocs, 1u);
  TEST_ASSERT(s.cycles > 0ULL);
}

/* ---------------------------------------------------------------------- */
/* Per-task getter                                                        */
/* ---------------------------------------------------------------------- */

static void test_task_stats_copies_from_tcb(void) {
  reset();
  TCB t = make_tcb(7, 3);
  t.perf.cycles_run = 12345;
  t.perf.max_burst_cyc = 678;
  t.perf.switches_in = 9;
  v_perf_task_t out = {0};
  v_perf_task_stats(&t, &out);
  TEST_ASSERT_EQ(out.cycles_run, 12345ULL);
  TEST_ASSERT_EQ(out.max_burst_cyc, 678u);
  TEST_ASSERT_EQ(out.switches_in, 9u);
}

/* Stack high-water: v_perf_task_stats measures peak usage by counting the
 * untouched V_PERF_STACK_FILL run at the bottom of the (downward-growing)
 * stack. 64-word (256 B) painted buffer; "growing the stack" = writing a
 * non-sentinel word at progressively lower indices. */
static void test_task_stack_highwater(void) {
  reset();
  uint32_t stack[64];
  for (int i = 0; i < 64; i++)
    stack[i] = V_PERF_STACK_FILL;
  TCB t = {0};
  t.task_id = 8;
  t.priority = 1;
  t.magic = TCB_MAGIC;
  t.mem_block = stack;
  t.stack_size = (uint32_t)sizeof(stack); /* 256 B */

  v_perf_task_t out = {0};
  v_perf_task_stats(&t, &out);
  TEST_ASSERT_EQ(out.stack_size, 256u);
  TEST_ASSERT_EQ(out.stack_peak, 0u); /* all sentinel -> nothing used yet */

  stack[40] = 0xDEADBEEFu; /* deepest used = word 40 */
  v_perf_task_stats(&t, &out);
  TEST_ASSERT_EQ(out.stack_peak, 96u); /* 256 - 40*4 */

  stack[8] = 0x12345678u; /* grew deeper -> high-water rises */
  v_perf_task_stats(&t, &out);
  TEST_ASSERT_EQ(out.stack_peak, 224u); /* 256 - 8*4 */
}

/* Stack high-water edges: a fully-used stack (no sentinel survives) reports
 * peak == size; a NULL mem_block degrades to peak == size without scanning. */
static void test_task_stack_highwater_edges(void) {
  reset();
  uint32_t stack[16];
  for (int i = 0; i < 16; i++)
    stack[i] = 0x11111111u; /* no V_PERF_STACK_FILL anywhere -> fully used */
  TCB t = {0};
  t.magic = TCB_MAGIC;
  t.mem_block = stack;
  t.stack_size = (uint32_t)sizeof(stack); /* 64 B */
  v_perf_task_t out = {0};
  v_perf_task_stats(&t, &out);
  TEST_ASSERT_EQ(out.stack_peak, 64u); /* nothing free -> all 64 B used */

  /* NULL mem_block: no scan, peak defaults to the full size, no crash. */
  TCB tn = {0};
  tn.magic = TCB_MAGIC;
  tn.mem_block = NULL;
  tn.stack_size = 100u;
  v_perf_task_stats(&tn, &out);
  TEST_ASSERT_EQ(out.stack_size, 100u);
  TEST_ASSERT_EQ(out.stack_peak, 100u);
}

static void test_task_stats_null_task_returns_zero(void) {
  v_perf_task_t out;
  out.cycles_run = 999;
  v_perf_task_stats(NULL, &out);
  TEST_ASSERT_EQ(out.cycles_run, 0ULL);
}

/* ---------------------------------------------------------------------- */
/* VFS-backed CSV dump                                                    */
/* ---------------------------------------------------------------------- */

static void test_dump_to_file_open_failure_returns_minus_one(void) {
  reset_with_current_task();
  vfs_stub_reset();
  vfs_stub.open_ret = -1;
  int rc = v_perf_dump_to_file("/sd/perf.csv");
  TEST_ASSERT_EQ(rc, -1);
  TEST_ASSERT_EQ(vfs_stub.write_called, 0);
  TEST_ASSERT_EQ(vfs_stub.close_called, 0);
}

static void test_dump_to_file_null_path_returns_minus_one(void) {
  reset();
  vfs_stub_reset();
  int rc = v_perf_dump_to_file(NULL);
  TEST_ASSERT_EQ(rc, -1);
  TEST_ASSERT_EQ(vfs_stub.open_called, 0);
}

static void test_dump_to_file_success_writes_csv(void) {
  reset_with_current_task();
  vfs_stub_reset();
  vfs_stub.open_ret = 3;
  vfs_stub.write_ret = 1;
  vfs_stub.close_ret = 0;
  int rc = v_perf_dump_to_file("/sd/perf.csv");
  TEST_ASSERT_EQ(rc, 0);
  TEST_ASSERT_EQ(vfs_stub.open_called, 1);
  TEST_ASSERT_EQ(vfs_stub.close_called, 1);
  /* The CSV has at least one line per subsystem header plus per-class
   * rows — well over 10 writes total. Asserting >10 keeps the schema
   * loose without coupling the test to exact line counts. */
  TEST_ASSERT(vfs_stub.write_called > 10);
}

static void test_dump_to_file_opens_truncating_write_only(void) {
  reset_with_current_task();
  vfs_stub_reset();
  vfs_stub.open_ret = 3;
  vfs_stub.write_ret = 1;
  v_perf_dump_to_file("/sd/x.csv");
  int flags = vfs_stub.open_flags;
  TEST_ASSERT((flags & VFS_O_WRONLY) != 0);
  TEST_ASSERT((flags & VFS_O_CREAT) != 0);
  TEST_ASSERT((flags & VFS_O_TRUNC) != 0);
}

/* ---------------------------------------------------------------------- */
/* Suite entrypoint                                                       */
/* ---------------------------------------------------------------------- */

void run_perf_tests(void) {
  TEST_SUITE_BEGIN("perf");
  TEST_RUN(test_reset_zeros_sched_switches);
  TEST_RUN(test_sched_switch_counter_monotonic);
  TEST_RUN(test_sched_per_task_accumulate);
  TEST_RUN(test_sched_idle_accounting);
  TEST_RUN(test_sched_max_burst_tracks_largest);
  TEST_RUN(test_isr_systick_min_max_last);
  TEST_RUN(test_isr_systick_preemption_counter);
  TEST_RUN(test_ipc_counters_independent);
  TEST_RUN(test_heap_alloc_per_class_bucketing);
  TEST_RUN(test_heap_free_coalesces_sum);
  TEST_RUN(test_heap_oom_separate_from_allocs);
  TEST_RUN(test_heap_peak_is_monotonic);
  TEST_RUN(test_snapshot_bundles_all_subsystems);
  TEST_RUN(test_task_stats_copies_from_tcb);
  TEST_RUN(test_task_stack_highwater);
  TEST_RUN(test_task_stack_highwater_edges);
  TEST_RUN(test_task_stats_null_task_returns_zero);
  TEST_RUN(test_dump_to_file_open_failure_returns_minus_one);
  TEST_RUN(test_dump_to_file_null_path_returns_minus_one);
  TEST_RUN(test_dump_to_file_success_writes_csv);
  TEST_RUN(test_dump_to_file_opens_truncating_write_only);
  TEST_SUITE_END();
}
