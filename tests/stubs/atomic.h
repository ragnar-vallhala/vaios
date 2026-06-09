#ifndef VAIOS_ATOMIC_H
#define VAIOS_ATOMIC_H
// Host-native stub for the port atomic header (shadows portable/<arch>/atomic.h
// on the host test build, the same way tests/stubs/port.h shadows port.h).
// The host suite is single-threaded, so plain accesses are sufficient.
#include <stdint.h>

typedef struct atomic {
  volatile int32_t counter;
} atomic_t;

static inline void atomic_set(atomic_t *v, int32_t i) { v->counter = i; }
static inline int32_t atomic_get(atomic_t *v) { return v->counter; }
static inline void atomic_inc(atomic_t *v) { v->counter++; }
static inline void atomic_dec(atomic_t *v) { v->counter--; }
#endif // VAIOS_ATOMIC_H
