#include "semihosting.h"
#include <stddef.h>
#include <stdint.h>

// Interrupt handling
#define SCB_SHPR1 (*(volatile uint32_t *)0xE000ED18)
#define SCB_SHPR2 (*(volatile uint32_t *)0xE000ED1C)
#define SCB_SHPR3 (*(volatile uint32_t *)0xE000ED20)

#ifndef __NVIC_PRIO_BITS /* normally from the Kconfig NVIC_PRIO_BITS (config) */
#define __NVIC_PRIO_BITS 4
#endif
#define PRIORITY_MASK ((1UL << __NVIC_PRIO_BITS) - 1)

void set_systick_interrupt_priority(
    uint32_t
        priority) { // Normalize to top 4 bits (0-15 effective priority levels)
  uint32_t prio = (priority & PRIORITY_MASK) << (8 - __NVIC_PRIO_BITS);

  SCB_SHPR3 = (SCB_SHPR3 & ~(0xFF << 24)) | (prio << 24);
}
void set_pendsv_interrupt_priority(
    uint32_t
        priority) { // Normalize to top 4 bits (0-15 effective priority levels)
  uint32_t prio = (priority & PRIORITY_MASK) << (8 - __NVIC_PRIO_BITS);

  SCB_SHPR3 = (SCB_SHPR3 & ~(0xFF << 16)) | (prio << 16);
}

static inline int semihosting_call(int reason, void *arg) {
  int value;
  __asm__ volatile("mov r0, %1\n" // reason code
                   "mov r1, %2\n" // argument pointer
                   "bkpt 0xAB\n"  // semihosting trap
                   "mov %0, r0\n" // return value in r0
                   : "=r"(value)
                   : "r"(reason), "r"(arg)
                   : "r0", "r1", "memory");
  return value;
}

void sh_write0(const char *s) { semihosting_call(SYS_WRITE0, (void *)s); }

void sh_putchar(char c) {
  char str[2] = {c, '\0'};
  sh_write0(str);
}

int sh_write(int fd, const void *buf, int len) {
  unsigned int args[3];
  args[0] = fd;
  args[1] = (uintptr_t)buf;
  args[2] = len;
  return semihosting_call(SYS_WRITE, args);
}

char sh_readc(void) { return (char)semihosting_call(SYS_READC, NULL); }

void sh_exit(int code) {
  // SYS_EXIT expects exit code in r1
  semihosting_call(SYS_EXIT, (void *)(uintptr_t)code);
  while (1) {
  } // Should never return
}

uint32_t sh_get_ticks(void) {
  return (uint32_t)semihosting_call(SYS_CLOCK, NULL);
}

// SYS_ELAPSED writes a 64-bit tick count into a two-word block and returns 0 on
// success (-1 if the host has no such counter). QEMU backs it with the virtual
// clock, so this is a genuine high-resolution monotonic time source under
// emulation — unlike SYS_CLOCK (10 ms granularity) it resolves individual
// operations.
uint64_t sh_elapsed(void) {
  volatile uint32_t words[2] = {0, 0};
  if (semihosting_call(SYS_ELAPSED, (void *)words) != 0)
    return 0; // unsupported by this host
  return ((uint64_t)words[1] << 32) | words[0];
}

uint32_t sh_tickfreq(void) {
  int f = semihosting_call(SYS_TICKFREQ, NULL);
  return (f < 0) ? 0u : (uint32_t)f; // -1 => host doesn't know the frequency
}
