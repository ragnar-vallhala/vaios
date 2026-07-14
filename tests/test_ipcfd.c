/**
 * @file test_ipcfd.c
 * @brief Regression tests for the fd-typed IPC teardown/name/recursion findings
 *        (STAGE5_REVIEW_FINDINGS #4, #7, #12), built with DEVFS + IPC_FD on and
 *        SVC off (the IPC bodies run directly against a real current_task).
 *
 * These assert the FIXED contract; they guard the fixes committed alongside.
 */
#include "framework.h"
#include "ipc.h"
#include "task.h"
#include "vfile.h"
#include <stdio.h>
#include <string.h>

extern void stub_reset_heap(void);
extern void stub_set_ticks(uint32_t t);
extern TCB *ready_lists[];
extern TCB *blocked_list;
extern TCB *delayed_list;
extern uint32_t ready_bitmap;
extern TCB *current_task;
extern TCB *idle_task;
extern uint32_t task_count;
extern uint32_t context_switch_count;

#define MAX_NAMED_SEMS 8 /* mirror the ipc.c table size */

static void full_reset(void) {
  stub_reset_heap();
  stub_set_ticks(0);
  for (int i = 0; i <= (int)MAX_PRIORITY; i++)
    ready_lists[i] = NULL;
  blocked_list = delayed_list = current_task = idle_task = NULL;
  ready_bitmap = 0;
  task_count = 0;
  context_switch_count = 0;
}

static void dummy_task(void *arg) {
  (void)arg;
  while (1)
    ;
}

/* Create a task at priority 3, make it the running task, return it (and its id
 * via *id_out). With the caller exiting each task before spawning the next,
 * ready_lists[3] holds exactly the new task. */
static TCB *spawn_current(uint32_t *id_out) {
  uint32_t id = task_create(dummy_task, NULL, 256, 3);
  if (id_out)
    *id_out = id;
  current_task = ready_lists[3];
  return current_task;
}

/* #4: a task that opens a named sem and exits without closing it must not leak
 * the table slot — exit closes its fds and drops the refcount. Repeatedly
 * open+exit with DISTINCT names more times than the table holds; the leak would
 * exhaust it and start returning -1. */
static void test_bug4_named_sem_slot_freed_on_exit(void) {
  full_reset();
  scheduler_init();
  for (int i = 0; i < MAX_NAMED_SEMS + 3; i++) {
    uint32_t id = 0;
    (void)spawn_current(&id);
    TEST_ASSERT(id != 0);
    char name[12];
    snprintf(name, sizeof name, "sem%d", i);
    int fd = v_sem_open(name, V_IPC_CREATE);
    TEST_ASSERT(fd >= 0); /* FAILS pre-fix once the leaked table fills */
    task_exit_request(id);
  }
}

/* #7: a name that doesn't fit the 16-byte slot (15 chars + NUL) must be rejected,
 * not silently truncated into a second, unreachable object. */
static void test_bug7_overlong_name_rejected(void) {
  full_reset();
  scheduler_init();
  (void)spawn_current(NULL);
  int too_long = v_sem_open("sensor_bus_north", V_IPC_CREATE); /* 16 chars */
  TEST_ASSERT(too_long < 0); /* FAILS pre-fix: silently created a truncated obj */
  int ok = v_sem_open("fifteenchars_ok", V_IPC_CREATE);        /* 15 chars */
  TEST_ASSERT(ok >= 0);
}

/* #12: a non-recursive fd mutex re-locked by its own owner must return an error,
 * not fall into the blocking path and self-deadlock. */
static void test_bug12_owner_relock_no_self_block(void) {
  full_reset();
  scheduler_init();
  TCB *t = spawn_current(NULL);
  int fd = v_mtx_open("m12", V_IPC_CREATE);
  TEST_ASSERT(fd >= 0);
  TEST_ASSERT_EQ(v_mtx_lock(fd, 0), VA_PASS); /* acquire */
  int r = v_mtx_lock(fd, 100);                /* owner re-lock, with a timeout */
  (void)r;
  /* FAILS pre-fix: the re-lock blocked the owner on itself. */
  TEST_ASSERT(t->status != TASK_BLOCKED);
}

static const test_case_t ipcfd_cases[] = {
    TEST_CASE(test_bug4_named_sem_slot_freed_on_exit),
    TEST_CASE(test_bug7_overlong_name_rejected),
    TEST_CASE(test_bug12_owner_relock_no_self_block),
};

const test_suite_t ipcfd_suite = {
    .name = "fd-typed IPC teardown/name/recursion (stage5 #4/#7/#12)",
    .cases = ipcfd_cases,
    .count = TEST_COUNT(ipcfd_cases),
};
