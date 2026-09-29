#include "memory.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <math.h>
#ifdef _FPU_ENABLED
#warning "FPU is ENABLED in this build"
#else
// #error "FPU is DISABLED in this build. Please build with -DVAIOS_FPU=ON"
#endif

void v_task_sine(void *arg) {
  float angle = 0.01f; // Start slightly above 0
  while (angle < 1000) {
    float s = sinf(angle);
    angle += 1.5f + s; //fabsf(s); 
    // v_log(LOG_INFO, "Task Sine: sin(%f) = %f", (double)angle, (float)s);
  }
  v_log(LOG_INFO, "Task Sine: Done");
}


void v_task_sqrt(void *arg) {
  float val = 1.0f;
  while (val < 1000) {
    float r = sqrtf(val);
    // v_log(LOG_INFO, "Task Sqrt: sqrt(%f) = %f", (double)val, (float)r);
    val += r;
  }
  v_log(LOG_INFO, "Task Sqrt: Done");
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_init(&cfg);
  v_heap_memory_init();
  scheduler_init();
  task_create(v_task_sine, NULL, 1024, 1);
  task_create(v_task_sqrt, NULL, 1024, 1);
  scheduler_start();

  while (1)
    ;
  return 0;
}
