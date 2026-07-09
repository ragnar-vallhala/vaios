/**
 * @file unit_tests.c
 * @brief On-target (processor-in-the-loop) unit-test runner.
 *
 * Links the host unit-test suites (tests/test_*.c) against the REAL kernel and
 * port and runs them on hardware / Renode, reporting results over the UART
 * console. This validates the actual silicon paths — real segregated heap,
 * real IPC mutexes/semaphores, real string formatter — that the host build
 * can only stub.
 *
 * Curated to the suites that need no scheduler, SD card, or DWT cycle counter
 * (so they run deterministically without a running scheduler or modelled
 * peripherals):
 *   - Memory allocator   (real v_malloc/v_free over the segregated heap)
 *   - SPSC / MPMC queues (real vaios mutexes + counting semaphores)
 *   - String formatter   (real print_fmt_buf / v_atof)
 *
 * Build:  cmake -DNAVHAL=ON -DEXAMPLES=ON -DVAIOS_EXAMPLE=UNIT_TESTS ..
 */
#include "framework.h"
#include "memory.h"
#include "suites.h"
#include "utils.h"
#include "vaios.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

/* Curated subset of the shared suite registry (suites.h). The pass/fail
 * counters and run_test_suites live in tests/framework.c, linked into this
 * target build. Only suites that need no scheduler / SD / DWT run here. */
static const test_suite_t *const target_suites[] = {
    &memory_suite,
    &structure_suite,
    &utils_suite,
};

/* The single test-harness hook the curated suites depend on. On the host this
 * lives in tests/stubs/stubs.c; on target it just re-inits the real heap. */
void stub_reset_heap(void) { v_heap_memory_init(); }

/* --------------------------------------------------------------------------
 * newlib retarget: route the framework's printf/fprintf to the UART console
 * (polling, via the port facade — never the DMA log path, so it is safe under
 * Renode). _sbrk backs any allocation newlib's stdio performs.
 * -------------------------------------------------------------------------- */
extern void v_print(const char *s);

int _write(int file, char *ptr, int len) {
  (void)file;
  char ch[2] = {0, 0};
  for (int i = 0; i < len; i++) {
    ch[0] = ptr[i];
    v_print(ch);
  }
  return len;
}

static char _arena[2048];
void *_sbrk(ptrdiff_t incr) {
  static char *brk = _arena;
  char *prev = brk;
  if (brk + incr > _arena + sizeof(_arena))
    return (void *)-1;
  brk += incr;
  return prev;
}

/* Remaining bare-metal syscall stubs newlib's stdio references (the other
 * examples don't use stdio, so nosys's set isn't otherwise pulled in). */
int _isatty(int file) {
  (void)file;
  return 1; /* treat the console as a tty */
}
int _fstat(int file, struct stat *st) {
  (void)file;
  st->st_mode = S_IFCHR;
  return 0;
}
int _close(int file) {
  (void)file;
  return -1;
}
int _read(int file, char *ptr, int len) {
  (void)file;
  (void)ptr;
  (void)len;
  return 0;
}
int _lseek(int file, int ptr, int dir) {
  (void)file;
  (void)ptr;
  (void)dir;
  return 0;
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_init(&cfg);         /* clocks + SysTick + UART console (no scheduler/SD) */
  v_heap_memory_init(); /* heap for the allocator and IPC objects */

  /* Unbuffered so each printf hits _write immediately (no stdio malloc). */
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);

  print_fmt("\r\n==== vaios ON-TARGET unit tests ====\r\n");

  run_test_suites(target_suites,
                  sizeof(target_suites) / sizeof(target_suites[0]));

  /* Greppable summary (print_fmt is the kernel formatter -> polling UART). */
  print_fmt("\r\n==== ON-TARGET TOTAL: %d passed, %d failed ====\r\n",
            _test_pass, _test_fail);
  print_fmt(_test_fail == 0 ? "ON-TARGET RESULT: ALL PASS\r\n"
                            : "ON-TARGET RESULT: FAIL\r\n");

  while (1) {
  }
}
