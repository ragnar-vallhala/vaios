#ifndef VAIOS_STRUCTURE_H
#define VAIOS_STRUCTURE_H

#include "ipc.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * SPSC FIFO (Single Producer, Single Consumer) - Lock-free
 * -------------------------------------------------------------------------- */

typedef enum {
  SPSC_POLICY_DROP = 0,     /* Default: drop new items if queue is full */
  SPSC_POLICY_OVERWRITE = 1 /* Overwrite oldest items if queue is full */
} spsc_policy_t;

typedef struct {
  void *buffer;
  size_t capacity;
  size_t elem_size;
  volatile size_t head;
  volatile size_t tail;
  spsc_policy_t policy;
#if VAIOS_MODULE_PERF
  /* Observability (VAIOS_MODULE_PERF): high-water fill and lost-item count.
   * `peak` is the max elements ever queued (size a buffer from this);
   * `drops` counts items lost to a full DROP fifo or overwritten under
   * OVERWRITE policy (backpressure signal). Read via spsc_peak/spsc_drops. */
  size_t peak;
  uint32_t drops;
#endif
} spsc_fifo_t;

/**
 * @brief Initialize SPSC FIFO.
 * @note buffer should be aligned to elem_size for optimal performance and
 * safety.
 * @note capacity must be at least 2.
 */
void spsc_init(spsc_fifo_t *f, void *buffer, size_t capacity, size_t elem_size);
void spsc_set_policy(spsc_fifo_t *f, spsc_policy_t policy);

size_t spsc_write(spsc_fifo_t *f, const void *items, size_t count);
size_t spsc_read(spsc_fifo_t *f, void *out, size_t count);
size_t spsc_available(const spsc_fifo_t *f); /* elements available to read */
size_t spsc_space(const spsc_fifo_t *f);     /* free slots available to write */
size_t spsc_peek(const spsc_fifo_t *f, void *out, size_t count);
size_t spsc_skip(spsc_fifo_t *f, size_t count);
void spsc_reset(spsc_fifo_t *f);
/* Observability getters — return 0 when VAIOS_MODULE_PERF is off. */
size_t spsc_peak(const spsc_fifo_t *f);   /* high-water fill (elements)     */
uint32_t spsc_drops(const spsc_fifo_t *f);/* items dropped/overwritten      */

/* Zero-copy support */
void *spsc_write_ptr(spsc_fifo_t *f, size_t *max_count);
void spsc_commit_write(spsc_fifo_t *f, size_t count);
void *spsc_read_ptr(spsc_fifo_t *f, size_t *max_count);
void spsc_commit_read(spsc_fifo_t *f, size_t count);

/* --------------------------------------------------------------------------
 * MPMC Queue (Multiple Producer, Multiple Consumer) - Thread-safe
 * -------------------------------------------------------------------------- */
typedef enum {
  MPMC_POLICY_DROP = 1,     /* Default: drop new items if queue is full */
  MPMC_POLICY_OVERWRITE = 2 /* Overwrite oldest items if queue is full */
} mpmc_policy_t;

typedef struct {
  void *buffer;
  size_t capacity;
  size_t elem_size;
  mpmc_policy_t policy;
  volatile size_t head;
  volatile size_t tail;
  volatile size_t count;

  MutexHandle_t lock;
  SemaphoreHandle_t not_empty;
  SemaphoreHandle_t not_full;
  /* fd-layer wake hints (M4), NULL unless the queue was registered for task
   * access with v_queue_register. Separate from not_empty/not_full, which ARE
   * this queue's element counts: a waiter must not consume those, or the counts
   * drift. These are binary "something changed" signals, given by whoever
   * pushes or pops — including a privileged ISR producer that never goes near
   * the fd layer, which is why they live here and not in the registry. */
  SemaphoreHandle_t fd_wake_rd;
  SemaphoreHandle_t fd_wake_wr;
} mpmc_queue_t;

void mpmc_init(mpmc_queue_t *q, void *buffer, size_t capacity,
               size_t elem_size);
void mpmc_set_policy(mpmc_queue_t *f, mpmc_policy_t policy);
bool mpmc_push(mpmc_queue_t *q, const void *item);     /* Blocking */
bool mpmc_pop(mpmc_queue_t *q, void *item);            /* Blocking */
bool mpmc_try_push(mpmc_queue_t *q, const void *item); /* Non-blocking */
bool mpmc_try_pop(mpmc_queue_t *q, void *item);        /* Non-blocking */
bool mpmc_push_timeout(mpmc_queue_t *q, const void *item, uint32_t timeout);
bool mpmc_pop_timeout(mpmc_queue_t *q, void *item, uint32_t timeout);

size_t mpmc_push_bulk(mpmc_queue_t *q, const void *items, size_t count);
size_t mpmc_pop_bulk(mpmc_queue_t *q, void *items, size_t max_count);

size_t mpmc_size(const mpmc_queue_t *q);
size_t mpmc_capacity(const mpmc_queue_t *q);
bool mpmc_is_empty(const mpmc_queue_t *q);
bool mpmc_is_full(const mpmc_queue_t *q);
bool mpmc_peek(mpmc_queue_t *q, void *item);
void mpmc_reset(mpmc_queue_t *q);

/* --------------------------------------------------------------------------
 * Queues on the fd table (M4) — how an unprivileged task reaches one.
 *
 * Both queues above are kernel memory: the MPMC one is built on a mutex and two
 * semaphores (raw handles the syscall layer refuses to take from a task), and
 * an SPSC fifo shared with an ISR lives in .bss, which a task cannot touch. So
 * the pattern is the one named semaphores and bus topics already use: privileged
 * init declares the queue and registers it under a name, and a task opens that
 * name and gets an fd.
 *
 * Prefer a bus PIPE TOPIC for new point-to-point code (include/bus.h): it is the
 * same lock-free ring with names, missed-message accounting and blocking recv.
 * The SPSC registration here exists for code that already holds an spsc_fifo_t.
 * -------------------------------------------------------------------------- */
#if VAIOS_DEVFS
#define V_Q_RD 0x1 /* this handle may receive */
#define V_Q_WR 0x2 /* this handle may send */

#define V_Q_EINVAL (-22) /* no such queue, wrong direction, bad argument */
#define V_Q_EBUSY (-16)  /* no free handle, or an SPSC side already taken */

/* Privileged, at init: publish a queue under `name` (a flash literal, kept by
 * pointer). Returns VA_PASS, or V_Q_EINVAL when the table is full or the name is
 * already taken. */
int v_queue_register(const char *name, mpmc_queue_t *q);
/* As above for an SPSC fifo. One reader and one writer only — the lock-free
 * property is the caller's promise, so the second of either is refused. */
int v_queue_register_spsc(const char *name, spsc_fifo_t *f);

/* Open a registered queue: fd >= 0, V_Q_EINVAL (no such queue / no direction),
 * V_Q_EBUSY (handle pool full, or that side of an SPSC is taken). Closing the fd
 * releases the handle; so does the task exiting. */
int v_queue_open(const char *name, int flags);
/* Send one element, copied from `item`. Its size is the QUEUE's elem_size — the
 * caller never states a length, so it cannot lie about one. ticks > 0 waits for
 * room (MPMC only; an SPSC queue has nothing to sleep on and returns VA_FAIL at
 * once). VA_PASS, VA_FAIL when full/timed out, V_Q_EINVAL on a bad handle. */
int v_queue_send(int fd, const void *item, uint32_t ticks);
/* Wait for room (for_write) or for an element, up to ticks. VA_PASS when the
 * queue is (or became) ready, VA_FAIL on timeout, V_Q_EINVAL on a bad handle or
 * an SPSC queue (which has nothing to sleep on). v_queue_send/recv are this plus
 * the non-blocking transfer, and are what callers normally want. */
int v_queue_wait(int fd, uint32_t ticks, int for_write);
/* Receive one element into `item` (elem_size bytes), waiting up to ticks. */
int v_queue_recv(int fd, void *item, uint32_t ticks);
/* The element size behind an fd, or negative. The syscall layer uses it to
 * bound-check the caller's buffer before copying either way. */
int v_queue_elem_size(int fd);
#endif /* VAIOS_DEVFS */

#ifdef __cplusplus
}
#endif

#endif /* VAIOS_STRUCTURE_H */
