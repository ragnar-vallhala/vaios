#ifndef VAIOS_UPIPE_H
#define VAIOS_UPIPE_H

#include <stdint.h>

/* v_upipe: a zero-copy, single-producer single-consumer ring shared by two
 * tasks (VAIOS_UPIPE). The ring sits in its own block, which the MPU maps
 * into exactly those two tasks, so the data path below runs entirely in user
 * space: no syscall, no copy, no kernel lock.
 *
 *   producer: s = v_upipe_reserve(p); fill s; v_upipe_commit(p);
 *   consumer: s = v_upipe_peek(p);    read s; v_upipe_release(p);
 *
 * reserve/peek return NULL on a full/empty ring; v_upipe_wait() sleeps until
 * that changes. commit/release trap into the kernel only when the peer is
 * asleep. Both ends live in shared memory, so a misbehaving peer can garble
 * the ring's contents -- never the kernel's or a third task's memory. */

typedef struct {
  volatile uint32_t wr;      // slots committed (free-running)
  volatile uint32_t rd;      // slots released (free-running)
  uint16_t slot;             // slot size, bytes (a multiple of 4)
  uint16_t n;                // slot count, a power of two
  volatile uint8_t wait_rd;  // the consumer is asleep, or about to be
  volatile uint8_t wait_wr;  // the producer is asleep, or about to be
  uint8_t pad[2];
  // n slots follow
} v_upipe_t;

/* Kernel side (privileged). Makes a pipe of `n` slots of `slot` bytes between
 * tasks `prod_id` and `cons_id` and maps it into both. NULL on bad arguments,
 * no memory, or a task that already has a pipe. */
v_upipe_t *v_upipe_create(uint32_t slot, uint32_t n, uint32_t prod_id,
                          uint32_t cons_id);

/* Syscalls. The kernel finds the pipe, and which end, from the caller. */
v_upipe_t *v_upipe_self(void); // the caller's pipe, NULL if none (yet)
int v_upipe_sleep(uint32_t ticks); // block until the peer kicks, or ticks
int v_upipe_kick(void);            // wake the peer

#define V_UPIPE_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST) // dmb

static inline void *v_upipe_slot(v_upipe_t *p, uint32_t i) {
  return (uint8_t *)(p + 1) + (i & (p->n - 1u)) * p->slot;
}

static inline void *v_upipe_reserve(v_upipe_t *p) {
  return p->wr - p->rd >= p->n ? 0 : v_upipe_slot(p, p->wr);
}

static inline void v_upipe_commit(v_upipe_t *p) {
  V_UPIPE_BARRIER(); // the slot's contents before the index
  p->wr++;
  V_UPIPE_BARRIER(); // the index before reading the peer's flag
  if (p->wait_rd)
    v_upipe_kick();
}

static inline const void *v_upipe_peek(v_upipe_t *p) {
  return p->wr == p->rd ? 0 : v_upipe_slot(p, p->rd);
}

static inline void v_upipe_release(v_upipe_t *p) {
  V_UPIPE_BARRIER();
  p->rd++;
  V_UPIPE_BARRIER();
  if (p->wait_wr)
    v_upipe_kick();
}

/* Sleep until reserve (for_write) or peek can succeed, or `ticks` pass.
 * Raises this end's flag first and re-checks, so a commit/release racing with
 * it either sees the flag and kicks, or is seen by the re-check. A kick that
 * lands before the sleep leaves the wake token set: no lost wakeup, at worst a
 * spurious one, so callers loop. */
static inline int v_upipe_wait(v_upipe_t *p, int for_write, uint32_t ticks) {
  volatile uint8_t *flag = for_write ? &p->wait_wr : &p->wait_rd;
  *flag = 1;
  V_UPIPE_BARRIER();
  int r = 0;
  if (for_write ? p->wr - p->rd >= p->n : p->wr == p->rd)
    r = v_upipe_sleep(ticks);
  *flag = 0;
  return r;
}

#endif
