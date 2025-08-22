#include "utils.h"
#include "vaios.h"
int main() {
  v_init();
  while (1)
    v_log(LOG_INFO, "Running fine");
}
