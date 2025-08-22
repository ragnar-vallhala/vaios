#include "utils.h"
#include <stdint.h>
#ifdef NAVHAL
#define CORTEX_M4
#include "navhal.h"
#endif

int main() {
#ifdef NAVHAL
  systick_init(10000);
  uart2_init(9600);
#endif

  while (1)
    v_log((v_get_ticks() / 500) % 7, "Everything is good %lu", v_get_ticks());
  // print_fmt("Hello World %u\n\r", (uint32_t)v_get_ticks());
}
