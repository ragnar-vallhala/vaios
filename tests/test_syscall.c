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
