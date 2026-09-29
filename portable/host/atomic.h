#ifndef VAIOS_ATOMIC_H
#define VAIOS_ATOMIC_H

// Host port atomics. Unlike the single-threaded tests/stubs/atomic.h, the
// running host port is preempted by the SIGALRM tick, so a counter touched by
// both task and tick context needs real atomicity — GCC/Clang __atomic builtins.
#include <stdint.h>

typedef struct atomic {
  volatile int32_t counter;
} atomic_t;

static inline void atomic_set(atomic_t *v, int32_t i) {
  __atomic_store_n(&v->counter, i, __ATOMIC_SEQ_CST);
}
static inline int32_t atomic_get(atomic_t *v) {
  return __atomic_load_n(&v->counter, __ATOMIC_SEQ_CST);
}
static inline void atomic_inc(atomic_t *v) {
  __atomic_fetch_add(&v->counter, 1, __ATOMIC_SEQ_CST);
}
static inline void atomic_dec(atomic_t *v) {
  __atomic_fetch_sub(&v->counter, 1, __ATOMIC_SEQ_CST);
}

#endif // VAIOS_ATOMIC_H
