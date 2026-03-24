#include "structure.h"
#include "port.h"
#include "utils.h"

/* --------------------------------------------------------------------------
 * SPSC FIFO Implementation
 * -------------------------------------------------------------------------- */

void spsc_init(spsc_fifo_t *f, void *buffer, size_t capacity,
               size_t elem_size) {
  uintptr_t addr = (uintptr_t)buffer;
  uintptr_t aligned_addr;

  /* Use power-of-2 optimization if applicable, otherwise fallback to generic */
  if (elem_size > 0 && (elem_size & (elem_size - 1)) == 0) {
    aligned_addr = (addr + (elem_size - 1)) & ~(elem_size - 1);
  } else {
    aligned_addr = (addr + elem_size - 1) / elem_size * elem_size;
  }

  size_t offset = aligned_addr - addr;

  if (offset > 0 && capacity > 0) {
    f->buffer = (void *)aligned_addr;
    f->capacity = capacity - 1;
  } else {
    f->buffer = buffer;
    f->capacity = capacity;
  }

  f->elem_size = elem_size;
  f->head = 0;
  f->tail = 0;
  f->policy = SPSC_POLICY_DROP;
}

void spsc_set_policy(spsc_fifo_t *f, spsc_policy_t policy) {
  f->policy = policy;
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
  if (count == 0 || f->capacity == 0)
    return 0;

  size_t avail = spsc_space(f);

  if (count > avail) {
    if (f->policy == SPSC_POLICY_DROP) {
      count = avail;
    } else {
      /* Overwrite policy: skip the number of items we are about to overwrite */
      size_t to_overwrite = count - avail;
      /* Cannot overwrite more than there is capacity for.
       * Leave one slot empty means max usable is capacity - 1 */
      if (to_overwrite >= f->capacity) {
        to_overwrite = f->capacity - 1;
      }
      spsc_skip(f, to_overwrite);
      /* After skip, avail should be exactly equal to 'count' (capped at
       * capacity-1) */
      if (count >= f->capacity) {
        count = f->capacity - 1;
      }
    }
  }

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
  V_PORT_MB();

  size_t new_head = head + count;
  if (new_head >= f->capacity)
    new_head -= f->capacity;
  f->head = new_head;

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
  V_PORT_MB();

  size_t new_tail = tail + count;
  if (new_tail >= f->capacity)
    new_tail -= f->capacity;
  f->tail = new_tail;

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

  size_t new_tail = f->tail + count;
  if (new_tail >= f->capacity)
    new_tail -= f->capacity;
  f->tail = new_tail;

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
  V_PORT_MB();

  size_t new_head = f->head + count;
  if (new_head >= f->capacity)
    new_head -= f->capacity;
  f->head = new_head;
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
  V_PORT_MB();

  size_t new_tail = f->tail + count;
  if (new_tail >= f->capacity)
    new_tail -= f->capacity;
  f->tail = new_tail;
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
  q->policy = MPMC_POLICY_DROP;
  q->not_empty = v_semaphore_create_counting(capacity, 0);
  q->not_full = v_semaphore_create_counting(capacity, capacity);
}

void mpmc_set_policy(mpmc_queue_t *f, mpmc_policy_t policy) {
  f->policy = policy;
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
  bool is_overwrite = (q->policy == MPMC_POLICY_OVERWRITE);
  uint32_t take_timeout = is_overwrite ? 0 : timeout;

  if (v_semaphore_take(q->not_full, take_timeout) != VA_PASS) {
    if (is_overwrite) {
      v_mutex_lock(q->lock, 0xFFFFFFFF);
      if (q->capacity == 0) {
        v_mutex_unlock(q->lock);
        return false;
      }
      if (q->count == q->capacity) {
        /* Overwrite oldest item */
        v_memcpy((uint8_t *)q->buffer + q->head * q->elem_size, item,
                 q->elem_size);
        q->head = (q->head + 1) % q->capacity;
        q->tail = (q->tail + 1) % q->capacity;
        /* count stays q->capacity */
        v_mutex_unlock(q->lock);
        /* No semaphore changes: not_empty is already at max, not_full is 0 */
        return true;
      }
      v_mutex_unlock(q->lock);
      /* If not full, someone popped between take and lock. Try again. */
      if (v_semaphore_take(q->not_full, 0) != VA_PASS) {
        /* This should be rare, but if it happens, we can't push normally.
         * Since it's not full, we could just lock and push, but we MUST
         * keep the semaphore in sync. Let's just return false or retry.
         * For reliability in MPMC, we'll try one more time with a small wait
         * if it's supposed to be an overwrite. */
        if (v_semaphore_take(q->not_full, 1) != VA_PASS)
          return false;
      }
    } else {
      return false;
    }
  }

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

  size_t next_tail = q->tail + 1;
  if (next_tail >= q->capacity)
    next_tail = 0;
  q->tail = next_tail;

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

size_t mpmc_size(const mpmc_queue_t *q) {
  v_mutex_lock(q->lock, 0xFFFFFFFF);
  size_t s = q->count;
  v_mutex_unlock(q->lock);
  return s;
}

size_t mpmc_capacity(const mpmc_queue_t *q) { return q->capacity; }

bool mpmc_is_empty(const mpmc_queue_t *q) { return mpmc_size(q) == 0; }

bool mpmc_is_full(const mpmc_queue_t *q) { return mpmc_size(q) == q->capacity; }

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

  /* Drain semaphores to reset state */
  while (v_semaphore_take(q->not_empty, 0) == VA_PASS)
    ;
  while (v_semaphore_take(q->not_full, 0) == VA_PASS)
    ;
  /* Restore not_full to capacity */
  for (size_t i = 0; i < q->capacity; i++) {
    v_semaphore_give(q->not_full);
  }

  v_mutex_unlock(q->lock);
}
