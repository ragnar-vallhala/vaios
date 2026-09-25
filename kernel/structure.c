#include "structure.h"
#include "port.h"
#include "utils.h"

/* --------------------------------------------------------------------------
 * SPSC FIFO Implementation
 * -------------------------------------------------------------------------- */

/* Defensive plausibility check against an spsc_fifo_t whose descriptor
 * has been corrupted by an upstream writer (see vayu task #13).
 * Returns true iff the descriptor's fields look like they could
 * possibly belong to a real queue.
 *
 * 4096 is a heuristic upper bound covering all real queues in vayu —
 * the biggest elem_size we ship is 76 (bmx160_all_reading_t) and the
 * largest capacity is around 11. If you ever need a queue beyond
 * those, bump these. NULL buffer also fails. */
static int _spsc_descriptor_sane(const spsc_fifo_t *f) {
  return f != 0 && f->buffer != 0 &&
         f->capacity > 0 && f->capacity <= 4096 &&
         f->elem_size > 0 && f->elem_size <= 4096;
}

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
#if VAIOS_MODULE_PERF
  f->peak = 0;
  f->drops = 0;
#endif
}

void spsc_set_policy(spsc_fifo_t *f, spsc_policy_t policy) {
  f->policy = policy;
}

size_t spsc_available(const spsc_fifo_t *f) {
  if (!_spsc_descriptor_sane(f))
    return 0;
  size_t head = f->head;
  size_t tail = f->tail;
  if (head >= f->capacity) head = head % f->capacity;
  if (tail >= f->capacity) tail = tail % f->capacity;
  if (head >= tail) {
    return head - tail;
  } else {
    return f->capacity - tail + head;
  }
}

size_t spsc_space(const spsc_fifo_t *f) {
  if (!_spsc_descriptor_sane(f))
    return 0;
  size_t head = f->head;
  size_t tail = f->tail;
  if (head >= f->capacity) head = head % f->capacity;
  if (tail >= f->capacity) tail = tail % f->capacity;
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
  if (count == 0 || !_spsc_descriptor_sane(f))
    return 0;

  size_t avail = spsc_space(f);

  if (count > avail) {
    if (f->policy == SPSC_POLICY_DROP) {
#if VAIOS_MODULE_PERF
      f->drops += (uint32_t)(count - avail); /* items the full fifo rejects */
#endif
      count = avail;
    } else {
      /* Overwrite policy: skip the number of items we are about to overwrite */
      size_t to_overwrite = count - avail;
      /* Cannot overwrite more than there is capacity for.
       * Leave one slot empty means max usable is capacity - 1 */
      if (to_overwrite >= f->capacity) {
        to_overwrite = f->capacity - 1;
      }
#if VAIOS_MODULE_PERF
      f->drops += (uint32_t)to_overwrite; /* old items lost to the consumer */
#endif
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
  /* Clamp head into [0, capacity) before using it for offset math. If some
   * upstream bug has corrupted f->head to a value >= capacity, the
   * unsigned subtraction `capacity - head` below would underflow to
   * SIZE_MAX and the next memcpy would write 30+ bytes at a wild
   * address. We clamp here to contain the blast radius — the queue's
   * own contents may still be wrong, but we won't trash unrelated RAM
   * or peripheral space. TODO(vayu): find the upstream writer that
   * corrupts head/tail in the first place; see vayu task #13. */
  size_t head = f->head;
  if (head >= f->capacity)
    head = head % f->capacity;
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

  /* Use modulo (not single subtract) so we wrap correctly even if the
   * starting head was already past capacity. */
  size_t new_head = (head + count) % f->capacity;
  f->head = new_head;

#if VAIOS_MODULE_PERF
  {
    size_t fill = spsc_available(f);
    if (fill > f->peak)
      f->peak = fill;
  }
#endif
  return count;
}

size_t spsc_read(spsc_fifo_t *f, void *out, size_t count) {
  if (!_spsc_descriptor_sane(f))
    return 0;
  size_t avail = spsc_available(f);
  if (count > avail)
    count = avail;
  if (count == 0)
    return 0;

  uint8_t *buf = (uint8_t *)f->buffer;
  /* Same defensive clamp as in spsc_write — see comment there. */
  size_t tail = f->tail;
  if (tail >= f->capacity)
    tail = tail % f->capacity;
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

  size_t new_tail = (tail + count) % f->capacity;
  f->tail = new_tail;

  return count;
}

size_t spsc_peek(const spsc_fifo_t *f, void *out, size_t count) {
  if (!_spsc_descriptor_sane(f))
    return 0;
  size_t avail = spsc_available(f);
  if (count > avail)
    count = avail;
  if (count == 0)
    return 0;

  uint8_t *buf = (uint8_t *)f->buffer;
  /* Same defensive clamp as in spsc_write — see comment there. */
  size_t tail = f->tail;
  if (tail >= f->capacity)
    tail = tail % f->capacity;
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
  if (!_spsc_descriptor_sane(f))
    return 0;
  size_t avail = spsc_available(f);
  if (count > avail)
    count = avail;

  /* Use modulo so we wrap correctly even if f->tail was already past
   * capacity (see spsc_write comment about the upstream corruption). */
  size_t new_tail = (f->tail + count) % f->capacity;
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
#if VAIOS_MODULE_PERF
  {
    size_t fill = spsc_available(f);
    if (fill > f->peak)
      f->peak = fill;
  }
#endif
}

/* Observability getters (0 when VAIOS_MODULE_PERF is compiled out). */
size_t spsc_peak(const spsc_fifo_t *f) {
#if VAIOS_MODULE_PERF
  return f ? f->peak : 0;
#else
  (void)f;
  return 0;
#endif
}

uint32_t spsc_drops(const spsc_fifo_t *f) {
#if VAIOS_MODULE_PERF
  return f ? f->drops : 0;
#else
  (void)f;
  return 0;
#endif
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
  if (q->fd_wake_rd) // a task sleeping in v_queue_recv, if any
    v_semaphore_give(q->fd_wake_rd);
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
  if (q->fd_wake_wr) // a task sleeping in v_queue_send, if any
    v_semaphore_give(q->fd_wake_wr);
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

// ---------------------------------------------------------------------------
// Queues on the fd table (M4). Privileged init registers a queue by name; a
// task opens that name and gets an fd. The queue's own storage, its mutex and
// its semaphores stay kernel-side, and only whole elements cross the boundary.
// ---------------------------------------------------------------------------
#if VAIOS_DEVFS
#include "syscall.h"
#include "vfile.h"

#define Q_REG_MAX 8

typedef struct {
  const char *name; // flash literal, kept by pointer (never copied)
  StaticSemaphore_t wake_rd, wake_wr; // storage for the queue's fd wake hints
  mpmc_queue_t *mpmc;
  spsc_fifo_t *spsc; // exactly one of the two is set
  uint8_t reader;    // SPSC only: its one reader / writer is taken
  uint8_t writer;
} q_entry_t;

static q_entry_t q_reg[Q_REG_MAX];

typedef struct {
  q_entry_t *entry;
  uint8_t used;
  uint8_t flags;
} q_handle_t;

static q_handle_t q_handles[VAIOS_QUEUE_MAX_OPEN];

static int q_name_eq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static q_entry_t *q_find(const char *name) {
  for (int i = 0; i < Q_REG_MAX; i++)
    if (q_reg[i].name && q_name_eq(q_reg[i].name, name))
      return &q_reg[i];
  return 0;
}

static int q_register(const char *name, mpmc_queue_t *m, spsc_fifo_t *f) {
  if (!name || !*name || (!m && !f))
    return V_Q_EINVAL;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  if (q_find(name)) { // a name means one queue, for the whole system
    EXIT_CRITICAL_FROM_ISR(s);
    return V_Q_EINVAL;
  }
  for (int i = 0; i < Q_REG_MAX; i++) {
    if (!q_reg[i].name) {
      q_reg[i].name = name;
      q_reg[i].mpmc = m;
      q_reg[i].spsc = f;
      q_reg[i].reader = q_reg[i].writer = 0;
      EXIT_CRITICAL_FROM_ISR(s);
      if (m) { // arm the wake hints: static storage, so no heap, no cleanup
        m->fd_wake_rd = v_semaphore_create_binary_static(&q_reg[i].wake_rd);
        m->fd_wake_wr = v_semaphore_create_binary_static(&q_reg[i].wake_wr);
      }
      return VA_PASS;
    }
  }
  EXIT_CRITICAL_FROM_ISR(s);
  return V_Q_EINVAL; // table full
}

int v_queue_register(const char *name, mpmc_queue_t *q) {
  return q_register(name, q, 0);
}
int v_queue_register_spsc(const char *name, spsc_fifo_t *f) {
  return q_register(name, 0, f);
}

static int q_fd_close(void *priv) {
  q_handle_t *h = (q_handle_t *)priv;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  if (h->entry && h->entry->spsc) { // give the SPSC side back
    if (h->flags & V_Q_RD)
      h->entry->reader = 0;
    if (h->flags & V_Q_WR)
      h->entry->writer = 0;
  }
  h->used = 0;
  h->entry = 0;
  EXIT_CRITICAL_FROM_ISR(s);
  return 0;
}

static const v_file_ops q_fd_ops = {
    .read = NULL, .write = NULL, .close = q_fd_close};

int v_queue_open(const char *name, int flags) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc2(SYS_q_open, (uintptr_t)name, (uint32_t)flags);
#endif
  if (!name || !(flags & (V_Q_RD | V_Q_WR)))
    return V_Q_EINVAL;
  q_entry_t *e = q_find(name);
  if (!e)
    return V_Q_EINVAL;

  q_handle_t *h = 0;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  // An SPSC queue is lock-free only while there is one of each side, and that
  // is the caller's promise to keep — so the kernel holds them to it.
  if (e->spsc && (((flags & V_Q_RD) && e->reader) ||
                  ((flags & V_Q_WR) && e->writer))) {
    EXIT_CRITICAL_FROM_ISR(s);
    return V_Q_EBUSY;
  }
  for (int i = 0; i < VAIOS_QUEUE_MAX_OPEN && !h; i++)
    if (!q_handles[i].used)
      h = &q_handles[i];
  if (h) {
    h->used = 1;
    h->entry = e;
    h->flags = (uint8_t)flags;
    if (e->spsc) {
      if (flags & V_Q_RD)
        e->reader = 1;
      if (flags & V_Q_WR)
        e->writer = 1;
    }
  }
  EXIT_CRITICAL_FROM_ISR(s);
  if (!h)
    return V_Q_EBUSY;

  int fd = v_fd_alloc(&q_fd_ops, h);
  if (fd < 0) {
    q_fd_close(h);
    return V_Q_EBUSY; // no free descriptor in this task
  }
  return fd;
}

int v_queue_elem_size(int fd) {
  q_handle_t *h = (q_handle_t *)v_fd_obj(fd, &q_fd_ops);
  if (!h || !h->entry)
    return V_Q_EINVAL;
  size_t n = h->entry->mpmc ? h->entry->mpmc->elem_size
                            : h->entry->spsc->elem_size;
  return n && n <= 0x7FFF ? (int)n : V_Q_EINVAL;
}

// One attempt, no waiting. A syscall body cannot block AND copy: blocking marks
// the task parked and returns V_SYSCALL_BLOCKED, and the kernel never runs again
// on that caller's behalf — the wake only writes a result into its stacked r0.
// So the retry loop CANNOT live in here. If it did, a transfer whose wait blocked
// would return the wait's success while having copied nothing, and the caller
// would read whatever its buffer held before (a stale duplicate, which is exactly
// how this was found on target). The loop therefore lives in the caller, below,
// out of trapping calls.
static int q_try(q_handle_t *h, void *item, int write) {
  if (h->entry->mpmc) {
    mpmc_queue_t *q = h->entry->mpmc;
    return (write ? mpmc_try_push(q, item) : mpmc_try_pop(q, item)) ? VA_PASS
                                                                    : VA_FAIL;
  }
  spsc_fifo_t *f = h->entry->spsc;
  size_t n = write ? spsc_write(f, item, 1) : spsc_read(f, item, 1);
  return n == 1 ? VA_PASS : VA_FAIL;
}

int v_queue_wait(int fd, uint32_t ticks, int for_write) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc3(SYS_q_wait, (uint32_t)fd, ticks, (uint32_t)for_write);
#endif
  q_handle_t *h = (q_handle_t *)v_fd_obj(fd, &q_fd_ops);
  if (!h || !h->entry)
    return V_Q_EINVAL;
  if (!(h->flags & (for_write ? V_Q_WR : V_Q_RD)))
    return V_Q_EINVAL;
  mpmc_queue_t *q = h->entry->mpmc;
  if (!q)
    return V_Q_EINVAL; // SPSC: a lock-free ring has nothing to sleep on
  // Ready already? Then do not sleep on a hint that may have been spent.
  if (for_write ? !mpmc_is_full(q) : !mpmc_is_empty(q))
    return VA_PASS;
  SemaphoreHandle_t wake = for_write ? q->fd_wake_wr : q->fd_wake_rd;
  if (!wake)
    return V_Q_EINVAL; // not registered for task access
  return v_semaphore_take(wake, ticks);
}

int v_queue_try_send(int fd, const void *item) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc2(SYS_q_send, (uint32_t)fd, (uintptr_t)item);
#endif
  q_handle_t *h = (q_handle_t *)v_fd_obj(fd, &q_fd_ops);
  if (!h || !item || !(h->flags & V_Q_WR))
    return V_Q_EINVAL;
  return q_try(h, (void *)item, 1);
}

int v_queue_try_recv(int fd, void *item) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc2(SYS_q_recv, (uint32_t)fd, (uintptr_t)item);
#endif
  q_handle_t *h = (q_handle_t *)v_fd_obj(fd, &q_fd_ops);
  if (!h || !item || !(h->flags & V_Q_RD))
    return V_Q_EINVAL;
  return q_try(h, item, 0);
}

// Try, then wait, then try again until the deadline — all from the CALLER's side,
// so every step is its own syscall and a wake always returns to a retry. The hint
// may be spurious or already spent and another task may take the slot first, so
// what bounds this is the deadline, not the signal.
static int q_transfer(int fd, void *item, uint32_t ticks, int write) {
  uint32_t start = v_get_ticks();
  for (;;) {
    int r = write ? v_queue_try_send(fd, item) : v_queue_try_recv(fd, item);
    if (r != VA_FAIL)
      return r; // moved it, or a real error (EINVAL)
    uint32_t spent = v_get_ticks() - start;
    if (spent >= ticks)
      return VA_FAIL; // full/empty (ticks == 0 lands here: a plain try)
    if (v_queue_wait(fd, ticks - spent, write) != VA_PASS)
      return VA_FAIL; // timed out, or nothing to wait on (SPSC)
  }
}

int v_queue_send(int fd, const void *item, uint32_t ticks) {
  return q_transfer(fd, (void *)item, ticks, 1);
}

int v_queue_recv(int fd, void *item, uint32_t ticks) {
  return q_transfer(fd, item, ticks, 0);
}
#endif // VAIOS_DEVFS
