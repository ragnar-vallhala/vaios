#include "structure.h"
#include "port.h"
#include "utils.h"
#include "vaios.h"

/* --------------------------------------------------------------------------
 * SPSC FIFO Implementation
 * -------------------------------------------------------------------------- */

void spsc_init(spsc_fifo_t *f, void *buffer, size_t capacity,
               size_t elem_size) {
  f->buffer = buffer;
  f->capacity = capacity;
  f->elem_size = elem_size;
  f->head = 0;
  f->tail = 0;
}

size_t spsc_available(const spsc_fifo_t *f) {
  size_t head = f->head;
  size_t tail = f->tail;
  if (head >= tail) {
    return head - tail;
  } else {
    return f->capacity - tail + head;
  }
}

size_t spsc_space(const spsc_fifo_t *f) {
  size_t head = f->head;
  size_t tail = f->tail;
  size_t free;
  if (head >= tail) {
    free = f->capacity - head + tail;
  } else {
    free = tail - head;
  }
  /* Always leave one slot empty to distinguish full from empty */
  return (free > 0) ? (free - 1) : 0;
}

size_t spsc_write(spsc_fifo_t *f, const void *items, size_t count) {
  size_t avail = spsc_space(f);
  if (count > avail)
    count = avail;
  if (count == 0)
    return 0;

  uint8_t *buf = (uint8_t *)f->buffer;
  size_t head = f->head;
  size_t to_end = f->capacity - head;

  if (count <= to_end) {
    v_memcpy(buf + head * f->elem_size, items, count * f->elem_size);
  } else {
    v_memcpy(buf + head * f->elem_size, items, to_end * f->elem_size);
    v_memcpy(buf, (uint8_t *)items + to_end * f->elem_size,
             (count - to_end) * f->elem_size);
  }

  /* Memory barrier to ensure data is written before head is updated */
  __asm__ volatile("dmb" : : : "memory");
  f->head = (head + count) % f->capacity;
  return count;
}

size_t spsc_read(spsc_fifo_t *f, void *out, size_t count) {
  size_t avail = spsc_available(f);
  if (count > avail)
    count = avail;
  if (count == 0)
    return 0;

  uint8_t *buf = (uint8_t *)f->buffer;
  size_t tail = f->tail;
  size_t to_end = f->capacity - tail;

  if (count <= to_end) {
    v_memcpy(out, buf + tail * f->elem_size, count * f->elem_size);
  } else {
    v_memcpy(out, buf + tail * f->elem_size, to_end * f->elem_size);
    v_memcpy((uint8_t *)out + to_end * f->elem_size, buf,
             (count - to_end) * f->elem_size);
  }

  /* Memory barrier to ensure data is read before tail is updated */
  __asm__ volatile("dmb" : : : "memory");
  f->tail = (tail + count) % f->capacity;
  return count;
}

size_t spsc_peek(const spsc_fifo_t *f, void *out, size_t count) {
  size_t avail = spsc_available(f);
  if (count > avail)
    count = avail;
  if (count == 0)
    return 0;

  uint8_t *buf = (uint8_t *)f->buffer;
  size_t tail = f->tail;
  size_t to_end = f->capacity - tail;

  if (count <= to_end) {
    v_memcpy(out, buf + tail * f->elem_size, count * f->elem_size);
  } else {
    v_memcpy(out, buf + tail * f->elem_size, to_end * f->elem_size);
    v_memcpy((uint8_t *)out + to_end * f->elem_size, buf,
             (count - to_end) * f->elem_size);
  }
  return count;
}

size_t spsc_skip(spsc_fifo_t *f, size_t count) {
  size_t avail = spsc_available(f);
  if (count > avail)
    count = avail;
  f->tail = (f->tail + count) % f->capacity;
  return count;
}

void spsc_reset(spsc_fifo_t *f) {
  f->head = 0;
  f->tail = 0;
}

void *spsc_write_ptr(spsc_fifo_t *f, size_t *max_count) {
  size_t head = f->head;
  size_t tail = f->tail;
  size_t free;

  if (head >= tail) {
    free = f->capacity - head;
    /* If tail is 0, we must leave one slot empty at the end if head is
     * capacity-1 */
    if (tail == 0 && free > 0)
      free--;
  } else {
    free = tail - head - 1;
  }

  *max_count = free;
  if (free == 0)
    return NULL;
  return (uint8_t *)f->buffer + head * f->elem_size;
}

void spsc_commit_write(spsc_fifo_t *f, size_t count) {
  if (count == 0)
    return;
  __asm__ volatile("dmb" : : : "memory");
  f->head = (f->head + count) % f->capacity;
}

void *spsc_read_ptr(spsc_fifo_t *f, size_t *max_count) {
  size_t head = f->head;
  size_t tail = f->tail;
  size_t avail;

  if (head >= tail) {
    avail = head - tail;
  } else {
    avail = f->capacity - tail;
  }

  *max_count = avail;
  if (avail == 0)
    return NULL;
  return (uint8_t *)f->buffer + tail * f->elem_size;
}

void spsc_commit_read(spsc_fifo_t *f, size_t count) {
  if (count == 0)
    return;
  __asm__ volatile("dmb" : : : "memory");
  f->tail = (f->tail + count) % f->capacity;
}

/* --------------------------------------------------------------------------
 * MPMC Queue Implementation
 * -------------------------------------------------------------------------- */

void mpmc_init(mpmc_queue_t *q, void *buffer, size_t capacity,
               size_t elem_size) {
  q->buffer = buffer;
  q->capacity = capacity;
  q->elem_size = elem_size;
  q->head = 0;
  q->tail = 0;
  q->count = 0;
  q->lock = v_mutex_create();
  q->not_empty = v_semaphore_create_counting(capacity, 0);
  q->not_full = v_semaphore_create_counting(capacity, capacity);
}

bool mpmc_push(mpmc_queue_t *q, const void *item) {
  return mpmc_push_timeout(q, item, 0xFFFFFFFF);
}

bool mpmc_pop(mpmc_queue_t *q, void *item) {
  return mpmc_pop_timeout(q, item, 0xFFFFFFFF);
}

bool mpmc_try_push(mpmc_queue_t *q, const void *item) {
  return mpmc_push_timeout(q, item, 0);
}

bool mpmc_try_pop(mpmc_queue_t *q, void *item) {
  return mpmc_pop_timeout(q, item, 0);
}

bool mpmc_push_timeout(mpmc_queue_t *q, const void *item, uint32_t timeout) {
  if (v_semaphore_take(q->not_full, timeout) != VA_PASS)
    return false;

  v_mutex_lock(q->lock, 0xFFFFFFFF);
  v_memcpy((uint8_t *)q->buffer + q->head * q->elem_size, item, q->elem_size);
  q->head = (q->head + 1) % q->capacity;
  q->count++;
  v_mutex_unlock(q->lock);

  v_semaphore_give(q->not_empty);
  return true;
}

bool mpmc_pop_timeout(mpmc_queue_t *q, void *item, uint32_t timeout) {
  if (v_semaphore_take(q->not_empty, timeout) != VA_PASS)
    return false;

  v_mutex_lock(q->lock, 0xFFFFFFFF);
  v_memcpy(item, (uint8_t *)q->buffer + q->tail * q->elem_size, q->elem_size);
  q->tail = (q->tail + 1) % q->capacity;
  q->count--;
  v_mutex_unlock(q->lock);

  v_semaphore_give(q->not_full);
  return true;
}

size_t mpmc_push_bulk(mpmc_queue_t *q, const void *items, size_t count) {
  size_t pushed = 0;
  const uint8_t *src = (const uint8_t *)items;
  while (pushed < count) {
    if (!mpmc_try_push(q, src + pushed * q->elem_size))
      break;
    pushed++;
  }
  return pushed;
}

size_t mpmc_pop_bulk(mpmc_queue_t *q, void *items, size_t max_count) {
  size_t popped = 0;
  uint8_t *dst = (uint8_t *)items;
  while (popped < max_count) {
    if (!mpmc_try_pop(q, dst + popped * q->elem_size))
      break;
    popped++;
  }
  return popped;
}

size_t mpmc_size(const mpmc_queue_t *q) { return q->count; }

size_t mpmc_capacity(const mpmc_queue_t *q) { return q->capacity; }

bool mpmc_is_empty(const mpmc_queue_t *q) { return q->count == 0; }

bool mpmc_is_full(const mpmc_queue_t *q) { return q->count == q->capacity; }

bool mpmc_peek(mpmc_queue_t *q, void *item) {
  v_mutex_lock(q->lock, 0xFFFFFFFF);
  if (q->count == 0) {
    v_mutex_unlock(q->lock);
    return false;
  }
  v_memcpy(item, (uint8_t *)q->buffer + q->tail * q->elem_size, q->elem_size);
  v_mutex_unlock(q->lock);
  return true;
}

void mpmc_reset(mpmc_queue_t *q) {
  v_mutex_lock(q->lock, 0xFFFFFFFF);
  q->head = 0;
  q->tail = 0;
  q->count = 0;
  /* Re-initialize semaphores is tricky, better to just drain them or use a
   * different approach */
  /* For simplicity, assume reset is for an empty queue or handles externally */
  v_mutex_unlock(q->lock);
}
