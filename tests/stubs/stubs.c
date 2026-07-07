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
 * Tick counter – tests can advance it by calling stub_set_ticks().
 * Named `systick_count` (not the old _stub_ticks) so vaios.c's v_delay,
 * which reads the global directly, resolves to this stub.
 * ---------------------------------------------------------------------- */
volatile uint32_t systick_count = 0;

void stub_set_ticks(uint32_t t) { systick_count = t; }
void stub_advance_ticks(uint32_t d) { systick_count += d; }

uint32_t v_get_ticks(void) { return systick_count; }

/* -------------------------------------------------------------------------
 * task_yield stub – counts calls so tests can inspect it
 * ---------------------------------------------------------------------- */
static int _yield_count = 0;
int stub_yield_count(void) { return _yield_count; }
void stub_reset_yield_count(void) { _yield_count = 0; }

void task_yield(void) { _yield_count++; }

/* Kernel-internal yield (pends PendSV directly on target). Count it too so the
 * refactored internal callers (task_delay/exit/block) keep the old semantics. */
void v_port_trigger_pendsv(void) { _yield_count++; }

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

/* kernel/perf.c v_perf_dump_to_file builds CSV lines via vaprint_fmt_buf.
 * On host we route to vsnprintf so the test path can exercise the writer
 * against a memory-backed VFS without pulling in the kernel's full formatter. */
int vaprint_fmt_buf(char *out, size_t out_size, const char *fmt, va_list ap) {
  int n = vsnprintf(out, out_size, fmt, ap);
  return n < 0 ? 0 : n;
}
int print_fmt_buf(char *out, uint32_t out_size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vaprint_fmt_buf(out, out_size, fmt, ap);
  va_end(ap);
  return n;
}

/* -------------------------------------------------------------------------
 * Utility function stubs – forward to libc equivalents
 * ---------------------------------------------------------------------- */
void *v_memset(void *s, int c, unsigned int n) { return memset(s, c, n); }

void *v_memcpy(void *dst, const void *src, unsigned int n) {
  return memcpy(dst, src, n);
}

uint32_t v_strlen(const char *s) { return (uint32_t)strlen(s); }

int v_strcmp(const char *s1, const char *s2) { return strcmp(s1, s2); }
int v_strncmp(const char *s1, const char *s2, int n) {
  return strncmp(s1, s2, (size_t)n);
}

/* -------------------------------------------------------------------------
 * v_panic stub — kernel code calls this on invariant violations
 * (heap watermark, invalid task pointer, stack overflow, etc.). In a host
 * test it means a real bug; print and abort so the test runner fails loudly.
 * ---------------------------------------------------------------------- */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------
 * Hardware bring-up and console I/O now live behind the port facade
 * (v_port_hw_*), stubbed for the host in tests/stubs/port_hw_stub.c. The
 * only kernel hardware symbol left for stubs.c is the DMA-completion
 * callback: vaios.c hands its address to v_port_hw_console_init(), so the
 * symbol must resolve even though utils.c (its real home) isn't linked into
 * vaios_tests. The host build never fires a DMA IRQ, so a no-op suffices.
 * ---------------------------------------------------------------------- */
void dma_tx_complete_callback(void) {}

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
