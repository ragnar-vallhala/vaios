#include "semihosting.h"
#include <stddef.h>

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
