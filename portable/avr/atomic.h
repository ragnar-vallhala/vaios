#ifndef VAIOS_ATOMIC_H
#define VAIOS_ATOMIC_H
// AVR atomic counter.
//
// On an 8-bit AVR even a 32-bit load/store is NOT atomic — it is several byte
// accesses an interrupt can split — so EVERY op runs with interrupts disabled.
// SREG is saved/restored (not a bare sei()) so the guard nests correctly and is
// safe to call from an ISR. This is the concrete reason the atomic impl is
// ISA-specific and lives in the port layer.
//
// (The AVR port is still nascent — this header is not built yet; it is here so
// the port-layer contract is complete when the AVR build lands.)
#include <avr/interrupt.h> // cli(), SREG
#include <stdint.h>

typedef struct atomic {
  volatile int32_t counter;
} atomic_t;

#define _VAIOS_ATOMIC_GUARD(body)                                              \
  do {                                                                         \
    uint8_t _sreg = SREG;                                                      \
    cli();                                                                     \
    body;                                                                      \
    SREG = _sreg;                                                              \
  } while (0)

static inline void atomic_set(atomic_t *v, int32_t i) {
  _VAIOS_ATOMIC_GUARD(v->counter = i);
}
static inline int32_t atomic_get(atomic_t *v) {
  int32_t r;
  _VAIOS_ATOMIC_GUARD(r = v->counter);
  return r;
}
static inline void atomic_inc(atomic_t *v) {
  _VAIOS_ATOMIC_GUARD(v->counter++);
}
static inline void atomic_dec(atomic_t *v) {
  _VAIOS_ATOMIC_GUARD(v->counter--);
}
#endif // VAIOS_ATOMIC_H
