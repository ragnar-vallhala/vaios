#include "ipc.h"
#include "memory.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>

/**
 * @file priority_inversion.c
 * @brief Improved example demonstrating priority inheritance.
 *
 * This example shows how priority inheritance allows a high-priority task (HPT)
 * to "help" a low-priority task (LPT) finish its work by boosting its priority,
 * thereby preventing a medium-priority task (MPT) from causing a hang.
 *
 * Scenario:
 * 1. LPT starts and acquires a mutex.
 * 2. MPT starts and enters an infinite busy loop.
 * 3. HPT starts and tries to acquire the same mutex.
 * 4. Priority Inheritance: HPT boosts LPT to Priority 3.
 * 5. LPT (now Priority 3) preempts MPT (Priority 2), finishes its work,
 *    and releases the mutex.
 * 6. HPT acquires the mutex and completes successfully.
 */

MutexHandle_t common_mtx;

// Helper to simulate CPU-bound work
void busy_wait(uint32_t iterations) {
  for (volatile uint32_t i = 0; i < iterations; i++)
    ;
}

/**
 * Low Priority Task (Priority 1)
 */
void lpt_task(void *arg) {
  v_log(LOG_INFO, "[LPT] Starting...");
  v_log_flush();

  v_log(LOG_INFO, "[LPT] Attempting to lock mutex...");
  v_log_flush();
  if (v_mutex_lock(common_mtx, 1000) == VA_PASS) {
    v_log(LOG_INFO, "[LPT] Mutex acquired!");
    v_log_flush();

    // Simulating work after acquiring mutex.
    // Initially LPT will be preempted by MPT.
    // Once HPT starts, LPT will be boosted and finish this loop.
    for (int i = 0; i < 5; i++) {
      busy_wait(1000000);
      v_log(LOG_INFO, "[LPT] Still working (Priority %u)...",
            (unsigned)get_current_task()->priority);
      v_log_flush();
      task_yield();
    }

    v_log(LOG_INFO, "[LPT] Releasing mutex...");
    v_log_flush();
    v_mutex_unlock(common_mtx);
  }

  while (1) {
    v_log_flush();
    task_yield();
  }
}

/**
 * Medium Priority Task (Priority 2)
 */
void mpt_task(void *arg) {
  v_log(LOG_INFO,
        "[MPT] Starting heavy CPU work (Preempts LPT if no inheritance)...");
  v_log_flush();

  // Infinite busy loop at Priority 2.
  while (1) {
    busy_wait(500000);
    // Only log periodically to avoid filling buffer
    static int count = 0;
    if (++count % 10 == 0) {
      v_log(LOG_INFO, "[MPT] Still running...");
      v_log_flush();
    }
    task_yield();
  }
}

/**
 * High Priority Task (Priority 3)
 */
void hpt_task(void *arg) {
  v_log(LOG_INFO, "[HPT] Starting, attempting to lock mutex...");
  v_log_flush();

  // This will block because LPT holds the mutex.
  // With Priority Inheritance, LPT will be boosted to Priority 3 and preempt
  // MPT.
  if (v_mutex_lock(common_mtx, 3000) == VA_PASS) {
    v_log(LOG_INFO,
          "[HPT] Mutex acquired successfully! Priority Inheritance working.");
    v_log_flush();
    v_mutex_unlock(common_mtx);
  } else {
    v_log(LOG_ERROR, "[HPT] Timeout! Priority Inheritance FAILED.");
    v_log_flush();
  }

  while (1) {
    v_log_flush();
    task_yield();
  }
}

void kernel_task(void *arg) {
  v_log(LOG_INFO, "=== Priority Inheritance Demo ===");
  v_log_flush();

  common_mtx = v_mutex_create();

  // 1. Create LPT first
  task_create(lpt_task, NULL, 1024, 1);
  task_delay(100);

  // 2. Create MPT next
  task_create(mpt_task, NULL, 1024, 2);
  task_delay(100);

  // 3. Create HPT last
  task_create(hpt_task, NULL, 1024, 3);

  while (1) {
    v_log_flush();
    task_delay(100);
  }
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_init(&cfg);
  v_heap_memory_init();
  scheduler_init();

  task_create(kernel_task, NULL, 2048, 4);

  scheduler_start();
  return 0;
}
