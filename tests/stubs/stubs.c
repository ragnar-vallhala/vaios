/**
 * @file stubs.c
 * @brief Host-native stub implementations for hardware-dependent functions.
 *
 * Provides: v_log, print, print_fmt, v_get_ticks (controllable tick counter),
 *           task_yield (call counter), init_task_stack, stub_reset_heap,
 *           _heap_start, v_memset, v_strlen, v_strcmp, v_log_flush,
 *           print_buffer_count.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Pull in the real type definitions (Log_Type, etc.) via utils.h.
 * utils.h also declares the static log_buffer arrays — that is fine here
 * because this translation unit is compiled as part of the host test binary,
 * not the kernel. */
#include "utils.h"

/* -------------------------------------------------------------------------
 * Tick counter – tests can advance it by calling stub_set_ticks()
 * ---------------------------------------------------------------------- */
static volatile uint32_t _stub_ticks = 0;

void stub_set_ticks(uint32_t t) { _stub_ticks = t; }
void stub_advance_ticks(uint32_t d) { _stub_ticks += d; }

uint32_t v_get_ticks(void) { return _stub_ticks; }

/* -------------------------------------------------------------------------
 * task_yield stub – counts calls so tests can inspect it
 * ---------------------------------------------------------------------- */
static int _yield_count = 0;
int stub_yield_count(void) { return _yield_count; }
void stub_reset_yield_count(void) { _yield_count = 0; }

void task_yield(void) { _yield_count++; }

/* -------------------------------------------------------------------------
 * init_task_stack stub – trivial host implementation
 * ---------------------------------------------------------------------- */
#include "task.h"

void init_task_stack(TCB *task) {
  /* On host we just need sp to point into the allocated buffer so that
   * task_create() does not crash.  No real exception frame is needed. */
  task->sp = task->mem_block;
}

/* -------------------------------------------------------------------------
 * _heap_start – backing storage for the kernel heap on the host.
 *
 * memory.c declares `extern uint32_t _heap_start;` and uses `&_heap_start`
 * as the base of a HEAP_SIZE-byte region. Declare a real HEAP_SIZE buffer
 * here, 8-byte aligned, so v_heap_memory_init()'s memset stays in-bounds
 * and stub_reset_heap() can just invoke the real init (which sets up the
 * one big free block AND the size-class free lists — both required by the
 * segregated allocator).
 * ---------------------------------------------------------------------- */
#include "memory.h"

uint32_t _heap_start[HEAP_SIZE / sizeof(uint32_t)]
    __attribute__((aligned(8)));

void stub_reset_heap(void) { v_heap_memory_init(); }

/* -------------------------------------------------------------------------
 * Logging stubs – silent; define STUB_VERBOSE to print
 * ---------------------------------------------------------------------- */
void v_log(Log_Type type, const char *msg, ...) {
#ifdef STUB_VERBOSE
  va_list args;
  va_start(args, msg);
  vprintf(msg, args);
  printf("\n");
  va_end(args);
#else
  (void)type;
  (void)msg;
#endif
}

void v_log_flush(void) {}

uint32_t print_buffer_count = 0;

void print(const char *str) { (void)str; }

void print_fmt(const char *fmt, ...) { (void)fmt; }

/* -------------------------------------------------------------------------
 * Utility function stubs – forward to libc equivalents
 * ---------------------------------------------------------------------- */
void *v_memset(void *s, int c, unsigned int n) { return memset(s, c, n); }

uint32_t v_strlen(const char *s) { return (uint32_t)strlen(s); }

int v_strcmp(const char *s1, const char *s2) { return strcmp(s1, s2); }

/* -------------------------------------------------------------------------
 * v_panic stub — kernel code calls this on invariant violations
 * (heap watermark, invalid task pointer, stack overflow, etc.). In a host
 * test it means a real bug; print and abort so the test runner fails loudly.
 * ---------------------------------------------------------------------- */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void v_panic(const char *file, int line, const char *fmt, ...) {
  fprintf(stderr, "\n[STUB v_panic] %s:%d: ", file ? file : "?", line);
  if (fmt) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
  }
  fputc('\n', stderr);
  abort();
}
