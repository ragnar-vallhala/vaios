#include "memory.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>


// Main
int main(void)
{
  v_init();
  heap_memory_init();
  v_log(LOG_INFO, "VaiOS initialized.\n");
  // v_start();
  while (1)
    ;

  return 0;
}
