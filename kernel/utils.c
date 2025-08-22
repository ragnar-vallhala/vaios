#include "utils.h"
#include "semihosting.h"
#ifdef NAVHAL
#define CORTEX_M4
#include "navhal.h"
#endif
void print(const char *str) {
#ifdef NAVHAL
  uart2_write(str);
#else
  sh_write0(str);
#endif
}
