#include "memory.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <math.h>
#ifdef _FPU_ENABLED
#warning "FPU is ENABLED in this build"
#else
#error "FPU is DISABLED in this build. Please build with -DVAIOS_FPU=ON"
#endif

void v_task_sine(void *arg) {
  float angle = 0.0f;
  while (1) {
    float s = sinf(angle);
    v_log(LOG_INFO, "Task Sine: sin(%f) = %f", (double)angle, (float)s);
    angle += 0.1f;
    if (angle > 6.28f)
      angle = 0.0f;
    v_delay(500);
  }
}

void v_task_sqrt(void *arg) {
  float val = 1.0f;
  while (1) {
    float r = sqrtf(val);
    v_log(LOG_INFO, "Task Sqrt: sqrt(%f) = %f", (double)val, (float)r);
    val += 1.5f;
    if (val > 100.0f)
      val = 1.0f;
    v_delay(700);
  }
}

int main(void) {
  v_init();
  v_heap_memory_init();
  scheduler_init();
  task_create(v_task_sine, NULL, 1024, 1);
  task_create(v_task_sqrt, NULL, 1024, 1);
  scheduler_start();

  while (1)
    ;
  return 0;
}
