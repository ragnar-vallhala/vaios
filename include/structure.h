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

/* Zero-copy support */
void *spsc_write_ptr(spsc_fifo_t *f, size_t *max_count);
void spsc_commit_write(spsc_fifo_t *f, size_t count);
void *spsc_read_ptr(spsc_fifo_t *f, size_t *max_count);
void spsc_commit_read(spsc_fifo_t *f, size_t count);

/* --------------------------------------------------------------------------
 * MPMC Queue (Multiple Producer, Multiple Consumer) - Thread-safe
 * -------------------------------------------------------------------------- */

typedef struct {
  void *buffer;
  size_t capacity;
  size_t elem_size;
  volatile size_t head;
  volatile size_t tail;
  volatile size_t count;
  MutexHandle_t lock;
  SemaphoreHandle_t not_empty;
  SemaphoreHandle_t not_full;
} mpmc_queue_t;

void mpmc_init(mpmc_queue_t *q, void *buffer, size_t capacity,
               size_t elem_size);
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

#ifdef __cplusplus
}
#endif

#endif /* VAIOS_STRUCTURE_H */
