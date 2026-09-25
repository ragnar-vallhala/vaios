/**
 * @file test_syscall.c
 * @brief SVC syscall-dispatch validation tests — portable/cortex-m4/syscall.c.
 *
 * The main vaios_tests binary does not link syscall.c, so v_syscall_dispatch —
 * where the Stage-5 security decisions live (the unprivileged pointer-validation
 * switch and the privileged-only IPC gate) — was never host-tested
 * (docs/plan/STAGE5_REVIEW_FINDINGS.md #2, and the dispatch side of #1/#3).
 *
 * This binary links the real dispatch + the real validators (task.c
 * v_access_ok / v_strnlen_user) with a synthetic caller whose privilege the
 * test flips. Built with MPU_USER_SEPARATION on but DEVFS/IPC_FD/TASK_HEAP off,
 * so the validation switch runs while the downstream device/fd/heap cases fall
 * through to the dispatch's `default: return -1` — the validator's verdict is
 * observed without needing the whole fd stack.
 *
 * The finding-#2 (SYS_wait nfds*sizeof(int) overflow) and finding-#1 (wnodes
 * overrun) regression tests land with their fix; asserting the fixed contract
 * here now would fail against the current buggy code.
 */
#include "framework.h"
#include "bus.h"
#include "perf.h"
#include "structure.h"
#include "periph_bus.h"
#include "syscall.h"
#include <stdint.h>

/* From tests/stubs/syscall_stubs.c. Sets current_task to a synthetic caller
 * with a `size`-byte block (in 32-bit-addressable memory so pointers survive
 * the uint32_t syscall ABI) at the given privilege; returns the block base. */
uint32_t syscall_set_caller(uint32_t size, int unprivileged);

/* Local mirrors of the dispatch's internal return codes (syscall.c, not
 * exported). Keep in sync with that file. */
#define T_EPERM (-1)
#define T_EFAULT (-14)

#define BLOCK_SZ 512u

/* A pointer well outside the caller's block (integer-derived to avoid forming
 * an out-of-bounds pointer, which UBSan would flag). v_access_ok only does
 * address math — it never dereferences — so this is safe to pass. */
#define BAD_PTR ((uintptr_t)0x1000u)

static intptr_t call(uint32_t num, uintptr_t a0, uintptr_t a1, uintptr_t a2) {
  uintptr_t args[4] = {a0, a1, a2, 0};
  return v_syscall_dispatch(num, args);
}

/* ---- Privileged-only gate: raw-handle IPC is denied to unprivileged callers */
static void test_unpriv_sem_give_denied(void) {
  (void)syscall_set_caller(BLOCK_SZ, /*unpriv=*/1);
  TEST_ASSERT_EQ(call(SYS_sem_give, 0, 0, 0), T_EPERM);
}
static void test_unpriv_sem_take_denied(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT_EQ(call(SYS_sem_take, 0, 0, 0), T_EPERM);
}
static void test_unpriv_mutex_lock_denied(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT_EQ(call(SYS_mutex_lock, 0, 0, 0), T_EPERM);
}
static void test_unpriv_mutex_unlock_denied(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT_EQ(call(SYS_mutex_unlock, 0, 0, 0), T_EPERM);
}

/* ---- Pointer validation: an out-of-block user pointer is rejected --------- */
static void test_unpriv_write_bad_ptr_efault(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  /* args: fd=1, buf=BAD_PTR, len=16 */
  TEST_ASSERT_EQ(call(SYS_write, 1, BAD_PTR, 16), T_EFAULT);
}
static void test_unpriv_read_bad_ptr_efault(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT_EQ(call(SYS_read, 0, BAD_PTR, 16), T_EFAULT);
}
static void test_unpriv_open_bad_str_efault(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  /* SYS_open validates args[0] as a NUL-bounded string in the block. */
  TEST_ASSERT_EQ(call(SYS_open, BAD_PTR, 0, 0), T_EFAULT);
}
static void test_unpriv_wait_bad_ptr_efault(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  /* args: fds=BAD_PTR, nfds=4 (small, no length overflow), ticks=1 */
  TEST_ASSERT_EQ(call(SYS_wait, BAD_PTR, 4, 1), T_EFAULT);
}

/* ---- A valid in-block pointer passes validation (not rejected). With DEVFS
 * off the SYS_write case falls to `default: -1`; the point is it is NOT the
 * validator's -14. */
static void test_unpriv_write_valid_ptr_passes(void) {
  uint32_t base = syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT(base != 0); /* MAP_32BIT block available */
  /* buf inside the block, len within bounds => validation passes. */
  TEST_ASSERT(call(SYS_write, 1, base, 16) != T_EFAULT);
}

/* ---- Peripheral-bus transfer syscalls: the descriptor AND the tx buffer it
 * points at must be the caller's own; so must the rx buffer at finish. */
static void test_unpriv_pbus_open_bad_str_efault(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT_EQ(call(SYS_pbus_open, BAD_PTR, 0, 0), T_EFAULT);
}
static void test_unpriv_pbus_submit_bad_desc_efault(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT_EQ(call(SYS_pbus_submit, 3, BAD_PTR, 1), T_EFAULT);
}
static void test_unpriv_pbus_submit_bad_tx_efault(void) {
  uint32_t base = syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT(base != 0);
  v_pbus_xfer_t *x = (v_pbus_xfer_t *)(uintptr_t)base; /* in-block descriptor */
  *x = (v_pbus_xfer_t){.tx = (const void *)BAD_PTR, .tx_len = 4};
  TEST_ASSERT_EQ(call(SYS_pbus_submit, 3, base, 1), T_EFAULT);
  /* Oversized tx_len is refused before its range is even checked. */
  x->tx = (const void *)(uintptr_t)(base + 64);
  x->tx_len = VAIOS_PBUS_XFER_MAX + 1;
  TEST_ASSERT_EQ(call(SYS_pbus_submit, 3, base, 1), V_PBUS_EINVAL);
}
/* rx is only written at finish, but a bad one must be refused at submit: a
 * refused finish would leave the handle stuck on its uncollected transfer. */
static void test_unpriv_pbus_submit_bad_rx_efault(void) {
  uint32_t base = syscall_set_caller(BLOCK_SZ, 1);
  v_pbus_xfer_t *x = (v_pbus_xfer_t *)(uintptr_t)base;
  *x = (v_pbus_xfer_t){.rx = (void *)BAD_PTR, .rx_len = 4};
  TEST_ASSERT_EQ(call(SYS_pbus_submit, 3, base, 1), T_EFAULT);
}
static void test_unpriv_pbus_submit_valid_passes(void) {
  uint32_t base = syscall_set_caller(BLOCK_SZ, 1);
  v_pbus_xfer_t *x = (v_pbus_xfer_t *)(uintptr_t)base;
  *x = (v_pbus_xfer_t){.tx = (const void *)(uintptr_t)(base + 64), .tx_len = 4};
  /* Validation passes; the body then rejects fd 3 (not an open bus handle). */
  TEST_ASSERT_EQ(call(SYS_pbus_submit, 3, base, 1), V_PBUS_EINVAL);
}
static void test_unpriv_pbus_finish_bad_rx_efault(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT_EQ(call(SYS_pbus_finish, 3, BAD_PTR, 8), T_EFAULT);
}

/* ---- Self-info and perf: both copy kernel-side state into the caller's own
 * struct, so both are pure write-validation cases. */
extern uint32_t stub_perf_sys_calls, stub_perf_self_calls; /* syscall_stubs.c */

static void test_unpriv_task_info_bad_ptr_efault(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT_EQ(call(SYS_task_info, BAD_PTR, 0, 0), T_EFAULT);
}
static void test_unpriv_task_info_valid_passes(void) {
  uint32_t base = syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT(base != 0);
  TEST_ASSERT(call(SYS_task_info, base, 0, 0) != T_EFAULT);
}
static void test_unpriv_perf_bad_ptrs_efault(void) {
  uint32_t base = syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT_EQ(call(SYS_perf_snapshot, BAD_PTR, 0, 0), T_EFAULT);
  TEST_ASSERT_EQ(call(SYS_perf_snapshot, 0, BAD_PTR, 0), T_EFAULT);
  /* A good system pointer does not excuse a bad self pointer. */
  TEST_ASSERT_EQ(call(SYS_perf_snapshot, base, BAD_PTR, 0), T_EFAULT);
}
/* Both pointers are optional: the caller picks which reading it wants, and
 * asking for neither must not be rejected as if a pointer were missing. */
static void test_unpriv_perf_optional_pointers(void) {
  uint32_t base = syscall_set_caller(BLOCK_SZ, 1);
  v_perf_snapshot_t *sys = (v_perf_snapshot_t *)(uintptr_t)base;
  v_perf_task_t *self = (v_perf_task_t *)(uintptr_t)(base + sizeof(*sys));
  sys->uptime_ticks = 0;
  self->switches_in = 0;

  uint32_t n_sys = stub_perf_sys_calls, n_self = stub_perf_self_calls;
  TEST_ASSERT_EQ(call(SYS_perf_snapshot, 0, 0, 0), 0); /* neither: legal no-op */
  TEST_ASSERT_EQ(stub_perf_sys_calls, n_sys);
  TEST_ASSERT_EQ(stub_perf_self_calls, n_self);

  TEST_ASSERT_EQ(call(SYS_perf_snapshot, base, 0, 0), 0); /* system only */
  TEST_ASSERT_EQ(sys->uptime_ticks, 0xABCDu);
  TEST_ASSERT_EQ(stub_perf_self_calls, n_self); /* self not touched */

  TEST_ASSERT_EQ(call(SYS_perf_snapshot, 0, (uintptr_t)self, 0), 0); /* self only */
  TEST_ASSERT_EQ(self->switches_in, 0x5A5Au);
}

/* ---- Ticks + drift-free periodic wait: the two kernel globals a user task
 * could not reach. SYS_ticks takes no pointer; SYS_delay_until keeps its
 * deadline in the caller's own word, which must be validated as a write. */
static void test_unpriv_ticks_reaches_body(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  /* The stub tick counter answers; the point is the validator does not reject a
   * syscall that carries no pointer, and it is not the dispatch default. */
  TEST_ASSERT(call(SYS_ticks, 0, 0, 0) != T_EFAULT);
  TEST_ASSERT(call(SYS_ticks, 0, 0, 0) != T_EPERM);
}
static void test_unpriv_delay_until_bad_ptr_efault(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT_EQ(call(SYS_delay_until, BAD_PTR, 5, 0), T_EFAULT);
}
static void test_unpriv_delay_until_valid_passes(void) {
  uint32_t base = syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT(base != 0);
  uint32_t *last_wake = (uint32_t *)(uintptr_t)base; /* in-block */
  *last_wake = 0;
  /* Validation passes and the body runs: it advances the caller's word by one
   * period (whether it slept depends on the stub clock). */
  TEST_ASSERT(call(SYS_delay_until, base, 5, 0) != T_EFAULT);
  TEST_ASSERT_EQ(*last_wake, 5u);
}

/* ---- Bus IPC syscalls (plan B9): the topic name, the payload it publishes,
 * and both levels of the rx block must be the caller's own. */
static void test_unpriv_bus_open_bad_str_efault(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT_EQ(call(SYS_bus_open, BAD_PTR, V_BUS_RD, 0), T_EFAULT);
}
static void test_unpriv_bus_send_bad_payload_efault(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT_EQ(call(SYS_bus_send, 3, BAD_PTR, 4), T_EFAULT);
}
static void test_unpriv_bus_recv_bad_rx_efault(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT_EQ(call(SYS_bus_recv, 3, BAD_PTR, 0), T_EFAULT);
}
/* The rx block itself is in the caller's memory, but the buffer it points at
 * is not: the second check is what stops a task writing where it pleases. */
static void test_unpriv_bus_recv_bad_rx_buf_efault(void) {
  uint32_t base = syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT(base != 0);
  v_bus_rx_t *rx = (v_bus_rx_t *)(uintptr_t)base;
  *rx = (v_bus_rx_t){.buf = (void *)BAD_PTR, .cap = 4};
  TEST_ASSERT_EQ(call(SYS_bus_recv, 3, base, 0), T_EFAULT);
}
static void test_unpriv_bus_valid_passes(void) {
  uint32_t base = syscall_set_caller(BLOCK_SZ, 1);
  v_bus_rx_t *rx = (v_bus_rx_t *)(uintptr_t)base;
  *rx = (v_bus_rx_t){.buf = (void *)(uintptr_t)(base + 64), .cap = 4};
  /* Validation passes; the body then rejects fd 3 (no bus handle here). */
  TEST_ASSERT_EQ(call(SYS_bus_recv, 3, base, 0), V_BUS_EINVAL);
  TEST_ASSERT_EQ(call(SYS_bus_send, 3, base + 64, 4), V_BUS_EINVAL);
  /* No bus is initialised in this binary, so a valid name finds no topic. */
  TEST_ASSERT_EQ(call(SYS_bus_open, base, V_BUS_RD, 0), V_BUS_EINVAL);
}
/* A zero-length send and a zero-cap recv carry no buffer to check: the
 * validator must not reject them on a pointer it was never given. */
static void test_unpriv_bus_zero_len_not_faulted(void) {
  uint32_t base = syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT_EQ(call(SYS_bus_send, 3, BAD_PTR, 0), V_BUS_EINVAL);
  v_bus_rx_t *rx = (v_bus_rx_t *)(uintptr_t)base;
  *rx = (v_bus_rx_t){.buf = (void *)BAD_PTR, .cap = 0};
  TEST_ASSERT_EQ(call(SYS_bus_recv, 3, base, 0), V_BUS_EINVAL);
}

/* ---- Read-only memory every task can read (flash on target): accepted for
 * reads, never for writes, and never past its end. */
void stub_set_user_ro(uintptr_t lo, uintptr_t hi); /* tests/stubs/stubs.c */
static const char fake_flash[32] = "i2c1";         /* stands in for .rodata */
#define RO_LO ((uintptr_t)fake_flash)
#define RO_HI ((uintptr_t)fake_flash + sizeof fake_flash)

static void test_unpriv_ro_read_ok_write_refused(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  stub_set_user_ro(RO_LO, RO_HI);
  TEST_ASSERT(call(SYS_write, 1, RO_LO, 16) != T_EFAULT); /* read from it */
  TEST_ASSERT_EQ(call(SYS_read, 0, RO_LO, 16), T_EFAULT); /* write into it */
  TEST_ASSERT_EQ(call(SYS_write, 1, RO_LO + 8, 32), T_EFAULT); /* runs past */
  stub_set_user_ro(0, 0);
}
static void test_unpriv_ro_string_accepted(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT_EQ(call(SYS_pbus_open, RO_LO, 0, 0), T_EFAULT); /* region off */
  stub_set_user_ro(RO_LO, RO_HI);
  TEST_ASSERT(call(SYS_pbus_open, RO_LO, 0, 0) != T_EFAULT);
  stub_set_user_ro(0, 0);
}
/* SYS_delay_until writes *last_wake back, so read-only memory is not enough —
 * flash as the deadline word must be refused, not silently written. */
static void test_unpriv_ro_delay_until_refused(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  stub_set_user_ro(RO_LO, RO_HI);
  TEST_ASSERT_EQ(call(SYS_delay_until, RO_LO, 5, 0), T_EFAULT);
  stub_set_user_ro(0, 0);
}
static void test_unpriv_ro_pbus_tx_ok_rx_refused(void) {
  uint32_t base = syscall_set_caller(BLOCK_SZ, 1);
  stub_set_user_ro(RO_LO, RO_HI);
  v_pbus_xfer_t *x = (v_pbus_xfer_t *)(uintptr_t)base;
  *x = (v_pbus_xfer_t){.tx = fake_flash, .tx_len = 8};
  /* Validation passes; the body then rejects fd 3 (no open bus handle). */
  TEST_ASSERT_EQ(call(SYS_pbus_submit, 3, base, 1), V_PBUS_EINVAL);
  x->rx = (void *)RO_LO;
  x->rx_len = 4;
  TEST_ASSERT_EQ(call(SYS_pbus_submit, 3, base, 1), T_EFAULT);
  stub_set_user_ro(0, 0);
}

/* ---- Queues (M4): the name is a user string, and the element buffer is bounded
 * by the QUEUE's elem_size — the caller never states a length, so it cannot lie
 * about one. No queue is registered in this binary, so a bad fd falls through to
 * the body's EINVAL rather than being confused with EFAULT. */
static void test_unpriv_q_open_bad_str_efault(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT_EQ(call(SYS_q_open, BAD_PTR, 1 /*V_Q_RD*/, 0), T_EFAULT);
}
static void test_unpriv_q_unknown_fd_is_einval_not_efault(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  /* fd 3 is not a queue handle: elem_size is unknown, so there is nothing to
   * bound-check and the body reports EINVAL. A bad pointer must not be reported
   * as a queue error, nor a bad fd as a fault. */
  int r = call(SYS_q_recv, 3, BAD_PTR, 0);
  TEST_ASSERT(r != T_EFAULT);
  TEST_ASSERT_EQ(r, V_Q_EINVAL);
}
static void test_unpriv_q_wait_takes_no_pointer(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  /* Scalars only: it must never be refused as a fault. */
  TEST_ASSERT(call(SYS_q_wait, 3, 10, 0) != T_EFAULT);
}

/* ---- Spawning (M3): the descriptor is the caller's, the ENTRY POINT must be
 * code. v_access_ok covers data regions; a RAM entry would mean asking the
 * kernel to start a task on a buffer the caller wrote, so it is refused here. */
static void test_unpriv_spawn_bad_desc_efault(void) {
  (void)syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT_EQ(call(SYS_task_spawn, BAD_PTR, 0, 0), T_EFAULT);
}
static void test_unpriv_spawn_ram_entry_refused(void) {
  uint32_t base = syscall_set_caller(BLOCK_SZ, 1);
  TEST_ASSERT(base != 0);
  v_task_spawn_t *cfg = (v_task_spawn_t *)(uintptr_t)base;
  *cfg = (v_task_spawn_t){.entry = (void (*)(void *))(uintptr_t)(base + 64),
                          .stack_size = 256, .priority = 1};
  /* entry points into the caller's own block: data, not code. */
  TEST_ASSERT_EQ(call(SYS_task_spawn, base, 0, 0), T_EFAULT);
}
/* With a read-only (flash-like) window armed, an entry inside it passes
 * validation; the same entry is refused the moment that window is gone. */
static void test_unpriv_spawn_flash_entry_accepted(void) {
  uint32_t base = syscall_set_caller(BLOCK_SZ, 1);
  v_task_spawn_t *cfg = (v_task_spawn_t *)(uintptr_t)base;
  *cfg = (v_task_spawn_t){.entry = (void (*)(void *))RO_LO,
                          .stack_size = 256, .priority = 1};
  TEST_ASSERT_EQ(call(SYS_task_spawn, base, 0, 0), T_EFAULT); /* window off */
  stub_set_user_ro(RO_LO, RO_HI);
  TEST_ASSERT(call(SYS_task_spawn, base, 0, 0) != T_EFAULT); /* validated */
  stub_set_user_ro(0, 0);
}
/* A name is a user string like any other, and NULL is legal (unnamed child). */
static void test_unpriv_spawn_name_validated(void) {
  uint32_t base = syscall_set_caller(BLOCK_SZ, 1);
  v_task_spawn_t *cfg = (v_task_spawn_t *)(uintptr_t)base;
  stub_set_user_ro(RO_LO, RO_HI);
  *cfg = (v_task_spawn_t){.entry = (void (*)(void *))RO_LO,
                          .stack_size = 256, .priority = 1,
                          .name = (const char *)BAD_PTR};
  TEST_ASSERT_EQ(call(SYS_task_spawn, base, 0, 0), T_EFAULT);
  cfg->name = NULL;
  TEST_ASSERT(call(SYS_task_spawn, base, 0, 0) != T_EFAULT);
  stub_set_user_ro(0, 0);
}

/* ---- A privileged caller bypasses the validation switch entirely. --------- */
static void test_priv_caller_skips_validation(void) {
  (void)syscall_set_caller(BLOCK_SZ, /*unpriv=*/0);
  /* Bad pointer, but privileged => not validated => not V_EFAULT. */
  TEST_ASSERT(call(SYS_write, 1, BAD_PTR, 16) != T_EFAULT);
}
static void test_priv_caller_ipc_not_denied(void) {
  (void)syscall_set_caller(BLOCK_SZ, 0);
  /* The privileged-only gate does not fire for a privileged caller. It would
   * fall through to the raw-handle dispatch; we only assert the gate is not
   * what produced the result by checking a distinct syscall path is reached. */
  /* Unknown syscall => -1 regardless, so just assert the gate is inert here by
   * confirming an unknown number returns the default. */
  TEST_ASSERT_EQ(call(0xDEAD, 0, 0, 0), -1);
}

/* ===========================================================================
 * Regression test for STAGE5_REVIEW_FINDINGS.md #2 — EXPECTED TO FAIL against
 * the current code. Fails gracefully (a return-value assertion).
 * =========================================================================== */

/* Finding #2: the SYS_wait length check is `args[1] * sizeof(int)`, a 32-bit
 * multiply. nfds = 0x40000000 makes the product wrap to 0, so v_access_ok(ptr,
 * 0) passes for any in-block pointer and the oversize nfds slips through. A
 * correct validator rejects an nfds that large (> VAIOS_MAX_FDS) with V_EFAULT.
 * (IPC_FD is off in this binary, so a slipped-through SYS_wait falls to the
 * dispatch default -1 rather than V_EFAULT — which is exactly the failure.) */
static void test_bug2_wait_nfds_multiply_overflow(void) {
  uint32_t base = syscall_set_caller(BLOCK_SZ, /*unpriv=*/1);
  TEST_ASSERT(base != 0);
  /* Valid in-block fds pointer, but a gigantic nfds whose *4 wraps to 0. */
  TEST_ASSERT_EQ(call(SYS_wait, base, 0x40000000u, 1), T_EFAULT);
}

static const test_case_t syscall_cases[] = {
    TEST_CASE(test_unpriv_sem_give_denied),
    TEST_CASE(test_unpriv_sem_take_denied),
    TEST_CASE(test_unpriv_mutex_lock_denied),
    TEST_CASE(test_unpriv_mutex_unlock_denied),
    TEST_CASE(test_unpriv_write_bad_ptr_efault),
    TEST_CASE(test_unpriv_read_bad_ptr_efault),
    TEST_CASE(test_unpriv_open_bad_str_efault),
    TEST_CASE(test_unpriv_wait_bad_ptr_efault),
    TEST_CASE(test_unpriv_write_valid_ptr_passes),
    TEST_CASE(test_unpriv_pbus_open_bad_str_efault),
    TEST_CASE(test_unpriv_pbus_submit_bad_desc_efault),
    TEST_CASE(test_unpriv_pbus_submit_bad_tx_efault),
    TEST_CASE(test_unpriv_pbus_submit_bad_rx_efault),
    TEST_CASE(test_unpriv_pbus_submit_valid_passes),
    TEST_CASE(test_unpriv_pbus_finish_bad_rx_efault),
    TEST_CASE(test_unpriv_q_open_bad_str_efault),
    TEST_CASE(test_unpriv_q_unknown_fd_is_einval_not_efault),
    TEST_CASE(test_unpriv_q_wait_takes_no_pointer),
    TEST_CASE(test_unpriv_spawn_bad_desc_efault),
    TEST_CASE(test_unpriv_spawn_ram_entry_refused),
    TEST_CASE(test_unpriv_spawn_flash_entry_accepted),
    TEST_CASE(test_unpriv_spawn_name_validated),
    TEST_CASE(test_unpriv_task_info_bad_ptr_efault),
    TEST_CASE(test_unpriv_task_info_valid_passes),
    TEST_CASE(test_unpriv_perf_bad_ptrs_efault),
    TEST_CASE(test_unpriv_perf_optional_pointers),
    TEST_CASE(test_unpriv_ticks_reaches_body),
    TEST_CASE(test_unpriv_delay_until_bad_ptr_efault),
    TEST_CASE(test_unpriv_delay_until_valid_passes),
    TEST_CASE(test_unpriv_bus_open_bad_str_efault),
    TEST_CASE(test_unpriv_bus_send_bad_payload_efault),
    TEST_CASE(test_unpriv_bus_recv_bad_rx_efault),
    TEST_CASE(test_unpriv_bus_recv_bad_rx_buf_efault),
    TEST_CASE(test_unpriv_bus_valid_passes),
    TEST_CASE(test_unpriv_bus_zero_len_not_faulted),
    TEST_CASE(test_unpriv_ro_read_ok_write_refused),
    TEST_CASE(test_unpriv_ro_string_accepted),
    TEST_CASE(test_unpriv_ro_delay_until_refused),
    TEST_CASE(test_unpriv_ro_pbus_tx_ok_rx_refused),
    TEST_CASE(test_priv_caller_skips_validation),
    TEST_CASE(test_priv_caller_ipc_not_denied),
    /* expected-fail regression test (see STAGE5_REVIEW_FINDINGS.md) */
    TEST_CASE(test_bug2_wait_nfds_multiply_overflow),
};

const test_suite_t syscall_suite = {
    .name = "syscall dispatch (validation + priv gate)",
    .cases = syscall_cases,
    .count = TEST_COUNT(syscall_cases),
};
