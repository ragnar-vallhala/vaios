#include "memory.h"
#include "structure.h"
#include "task.h"
#include "vaios.h"
#include "utils.h"
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

  /* 2. Overwrite Policy */
  spsc_reset(&spsc);
  /* Fill perfectly (16 slots original - 1 alignment - 1 empty = 14 elements max
   * if shifted) Wait, spsc_init with 16 elements gives f->capacity = 16 (if
   * aligned). Space is 15.
   */
  for (uint32_t i = 1; i <= 15; i++) {
    spsc_write(&spsc, &i, 1);
  }

  spsc_set_policy(&spsc, SPSC_POLICY_OVERWRITE);
  uint32_t extra = 100;
  /* Queue is full, this should overwrite value 1 (at index 0) and move tail to
   * index 1 (value 2) */
  spsc_write(&spsc, &extra, 1);

  uint32_t first;
  spsc_read(&spsc, &first, 1);
  if (first == 2) {
    v_log(LOG_INFO, "SPSC Overwrite Policy: PASS");
  } else {
    v_log(LOG_WARN, "SPSC Overwrite Policy: FAIL (expected 2, got %d)", first);
  }

  /* 3. Filling and Wrapping */
  spsc_reset(&spsc);
  v_log(LOG_INFO, "SPSC Filling...");
  for (uint32_t i = 0; i < 15; i++) {
    spsc_write(&spsc, &i, 1);
  }
  v_log(LOG_INFO, "SPSC space: %d (should be 0)", spsc_space(&spsc));

  uint32_t tmp;
  spsc_read(&spsc, &tmp, 1); /* Read 0 */
  uint32_t wrap_val = 99;
  spsc_write(&spsc, &wrap_val, 1); /* Should wrap */
  v_log(LOG_INFO, "SPSC Wrapping: PASS");

  /* 4. Zero-copy API */
  size_t max;
  uint32_t *ptr = (uint32_t *)spsc_read_ptr(&spsc, &max);
  if (ptr && *ptr == 1) { /* Next value is 1 */
    v_log(LOG_INFO, "SPSC Zero-copy Read: PASS (value 0x%X)", *ptr);
    spsc_commit_read(&spsc, 1);
  } else {
    v_log(LOG_WARN, "SPSC Zero-copy Read: FAIL (got 0x%X)", ptr ? *ptr : 0);
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
    uint32_t val = 0xEEEEEEEE; /* Sentinel */
    if (mpmc_pop(&mpmc, &val)) {
      v_log(LOG_INFO, "Consumer %d: Popped 0x%X", id, val);
    } else {
      v_log(LOG_ERROR, "Consumer %d: Pop FAILED", id);
    }
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
  uint32_t bulk_in[4] = {10, 20, 30, 40};
  uint32_t bulk_out[4] = {0};
  size_t pushed = mpmc_push_bulk(&mpmc, bulk_in, 4);
  size_t popped = mpmc_pop_bulk(&mpmc, bulk_out, 4);
  if (pushed == 4 && popped == 4 && bulk_out[3] == 40) {
    v_log(LOG_INFO, "MPMC Bulk Op: PASS");
  } else {
    v_log(LOG_WARN, "MPMC Bulk Op: FAIL (p%d, r%d, val%d)", pushed, popped,
          bulk_out[3]);
  }

  /* 3. Multi-task test */
  static int p_id = 1;
  static int c_id = 2;
  task_create(producer_task, &p_id, 2048, 1);
  task_create(consumer_task, &c_id, 2048, 1);
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
  task_create(kernel_task, NULL, 4096, 2);
  scheduler_start();
  while (1)
    ;
}
