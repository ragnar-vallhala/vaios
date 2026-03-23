#include "memory.h"
#include "structure.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>

#define FIFO_CAPACITY 16
#define ELEM_SIZE sizeof(uint32_t)

static uint32_t spsc_buf[FIFO_CAPACITY];
static uint32_t mpmc_buf[FIFO_CAPACITY];
static spsc_fifo_t spsc;
static mpmc_queue_t mpmc;

/* --------------------------------------------------------------------------
 * SPSC Test
 * -------------------------------------------------------------------------- */
void test_spsc(void) {
  v_log(LOG_INFO, "--- Testing SPSC FIFO ---");
  spsc_init(&spsc, spsc_buf, FIFO_CAPACITY, ELEM_SIZE);

  /* 1. Basic write/read */
  uint32_t val1 = 0xDEADBEEF;
  uint32_t val2 = 0xCAFEBABE;
  spsc_write(&spsc, &val1, 1);
  spsc_write(&spsc, &val2, 1);

  uint32_t out1 = 0, out2 = 0;
  spsc_read(&spsc, &out1, 1);
  spsc_read(&spsc, &out2, 1);

  if (out1 == val1 && out2 == val2) {
    v_log(LOG_INFO, "SPSC Basic R/W: PASS");
  } else {
    v_log(LOG_ERROR, "SPSC Basic R/W: FAIL (0x%X, 0x%X)", out1, out2);
  }

  /* 2. Filling and Wrapping */
  v_log(LOG_INFO, "SPSC Filling...");
  for (uint32_t i = 0; i < FIFO_CAPACITY - 1; i++) {
    spsc_write(&spsc, &i, 1);
  }
  v_log(LOG_INFO, "SPSC space: %d (should be 0)", spsc_space(&spsc));

  uint32_t tmp;
  spsc_read(&spsc, &tmp, 1); /* Read one to make space */
  uint32_t wrap_val = 99;
  spsc_write(&spsc, &wrap_val, 1); /* Should wrap */

  v_log(LOG_INFO, "SPSC Wrapping: PASS (if no crash)");

  /* 3. Zero-copy API */
  size_t max;
  uint32_t *ptr = (uint32_t *)spsc_write_ptr(&spsc, &max);
  if (ptr) {
    *ptr = 0x12345678;
    spsc_commit_write(&spsc, 1);
    v_log(LOG_INFO, "SPSC Zero-copy Write: PASS");
  }

  ptr = (uint32_t *)spsc_read_ptr(&spsc, &max);
  if (ptr) {
    v_log(LOG_INFO, "SPSC Zero-copy Read value: 0x%X", *ptr);
    spsc_commit_read(&spsc, 1);
  }
}

/* --------------------------------------------------------------------------
 * MPMC Test
 * -------------------------------------------------------------------------- */
static void producer_task(void *arg) {
  int id = *(int *)arg;
  for (uint32_t i = 0; i < 5; i++) {
    uint32_t val = (id << 16) | i;
    v_log(LOG_INFO, "Producer %d: Pushing 0x%X", id, val);
    mpmc_push(&mpmc, &val);
    v_delay(10);
  }
  v_log(LOG_INFO, "Producer %d: Done", id);
}

static void consumer_task(void *arg) {
  int id = *(int *)arg;
  for (uint32_t i = 0; i < 5; i++) {
    uint32_t val;
    mpmc_pop(&mpmc, &val);
    v_log(LOG_INFO, "Consumer %d: Popped 0x%X", id, val);
    v_delay(15);
  }
  v_log(LOG_INFO, "Consumer %d: Done", id);
}

void test_mpmc(void) {
  v_log(LOG_INFO, "--- Testing MPMC Queue ---");
  mpmc_init(&mpmc, mpmc_buf, FIFO_CAPACITY, ELEM_SIZE);

  /* 1. Try push/pop */
  uint32_t val = 0xABCDEF01;
  if (mpmc_try_push(&mpmc, &val)) {
    uint32_t out;
    if (mpmc_try_pop(&mpmc, &out) && out == val) {
      v_log(LOG_INFO, "MPMC Try Push/Pop: PASS");
    }
  }

  /* 2. Bulk operations */
  uint32_t bulk_in[4] = {1, 2, 3, 4};
  uint32_t bulk_out[4] = {0};
  size_t pushed = mpmc_push_bulk(&mpmc, bulk_in, 4);
  size_t popped = mpmc_pop_bulk(&mpmc, bulk_out, 4);
  if (pushed == 4 && popped == 4 && bulk_out[3] == 4) {
    v_log(LOG_INFO, "MPMC Bulk Op: PASS");
  }

  /* 3. Multi-task test */
  static int p_id = 1;
  static int c_id = 2;
  task_create(producer_task, &p_id, 1024, 1);
  task_create(consumer_task, &c_id, 1024, 1);
}

void kernel_task(void *arg) {
  (void)arg;

  test_spsc();
  test_mpmc();

  while (1) {
    v_delay(1000);
  }
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_init(&cfg);
  v_heap_memory_init();
  scheduler_init();

  task_create(kernel_task, NULL, 2048, 2);

  scheduler_start();
  while (1)
    ;
}
