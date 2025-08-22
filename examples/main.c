#include "utils.h"
#ifdef NAVHAL
#define CORTEX_M4
#include "navhal.h"
#endif
int main() {
#ifdef NAVHAL
  uart2_init(9600);
#endif
  print("Hello World\n\r");
}
