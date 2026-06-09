#ifndef VAIOS_ATOMIC_H
#define VAIOS_ATOMIC_H
// Cortex-M4 atomic counter.
//
// RMW (inc/dec) is lock-free via LDREX/STREX, so it takes NO critical section:
// it is safe from any context and never touches the global `critical_nesting`
// (which is what let an ISR-side give corrupt a preempted task's accounting).
// A single aligned 32-bit load/store is already atomic on this core, so get/set
// are plain volatile accesses.
#include "port.h" // v_port_ldrex / v_port_strex
#include <stdint.h>

typedef struct atomic {
  volatile int32_t counter;
} atomic_t;

static inline void atomic_set(atomic_t *v, int32_t i) { v->counter = i; }
static inline int32_t atomic_get(atomic_t *v) { return v->counter; }

static inline void atomic_inc(atomic_t *v) {
  uint32_t old;
  do {
    old = v_port_ldrex((volatile uint32_t *)&v->counter);
  } while (v_port_strex(old + 1u, (volatile uint32_t *)&v->counter) != 0u);
}

static inline void atomic_dec(atomic_t *v) {
  uint32_t old;
  do {
    old = v_port_ldrex((volatile uint32_t *)&v->counter);
  } while (v_port_strex(old - 1u, (volatile uint32_t *)&v->counter) != 0u);
}
#endif // VAIOS_ATOMIC_H
