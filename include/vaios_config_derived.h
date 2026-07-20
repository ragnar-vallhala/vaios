#ifndef VAIOS_CONFIG_DERIVED_H
#define VAIOS_CONFIG_DERIVED_H
/*
 * Derived / composite configuration macros.
 *
 * The Kconfig-generated vaios_autoconf.h (force-included ahead of every TU)
 * provides the scalar knobs. The macros here are the ones that are expressions,
 * fixed constants, or function-like — i.e. not plain Kconfig scalars — so they
 * are built in C from those scalars rather than emitted by the generator. This
 * is the small, hand-written glue layer every Kconfig project keeps.
 *
 * All definitions are #ifndef-guarded so a command-line -D still wins.
 */

/* NOTE: the NVIC priority model (__NVIC_PRIO_BITS, MAX_SYSCALL_INTERRUPT_PRIORITY)
 * used to live here. It is ARMv7-M-specific — an 8-bit, high-bit-justified
 * priority register with lower-is-more-urgent ordering — so it moved into the
 * port (portable/cortex-m4/port.h, mirrored by tests/stubs/port.h). This header
 * stays arch-neutral. */

/* Heap allocator selector. VAIOS_HEAP_SEGLIST / VAIOS_HEAP_TLSF are fixed index
 * constants; VAIOS_HEAP_ALGO is the active one. The seglist.c / tlsf.c bodies
 * gate on `#if VAIOS_HEAP_ALGO == VAIOS_HEAP_SEGLIST`. The active backend comes
 * from the Kconfig `choice` (VAIOS_HEAP_ALGO_SEGLIST / _TLSF). */
#ifndef VAIOS_HEAP_SEGLIST
#define VAIOS_HEAP_SEGLIST 0
#endif
#ifndef VAIOS_HEAP_TLSF
#define VAIOS_HEAP_TLSF 1
#endif
#ifndef VAIOS_HEAP_ALGO
#if defined(VAIOS_HEAP_ALGO_TLSF) && VAIOS_HEAP_ALGO_TLSF
#define VAIOS_HEAP_ALGO VAIOS_HEAP_TLSF
#else
#define VAIOS_HEAP_ALGO VAIOS_HEAP_SEGLIST
#endif
#endif

/* Panic helper — captures file/line at the call site. */
#ifndef PANIC
#define PANIC(msg) v_panic(__FILE__, __LINE__, msg)
#endif

#endif /* VAIOS_CONFIG_DERIVED_H */
