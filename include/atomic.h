#ifndef VAIOS_ATOMIC_H
#define VAIOS_ATOMIC_H
#include "port.h"
#include <stdint.h>
typedef struct atomic {
  volatile int32_t counter;
} atomic_t;

static inline void atomic_set(atomic_t *v, int32_t i) {
  ENTER_CRITICAL();
  v->counter = i;
  EXIT_CRITICAL();
}
static inline int32_t atomic_get(atomic_t *v) {
  int32_t val;
  ENTER_CRITICAL();
  val = v->counter;
  EXIT_CRITICAL();
  return val;
}

static inline void atomic_inc(atomic_t *v) {
  ENTER_CRITICAL();
  v->counter++;
  EXIT_CRITICAL();
}
static inline void atomic_dec(atomic_t *v) {
  ENTER_CRITICAL();
  v->counter--;
  EXIT_CRITICAL();
}
#endif // VAIOS_ATOMIC_H