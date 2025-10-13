#include "vaios.h"
#include "ipc.h"
#include "task.h"
#include "utils.h"
#include "memory.h"
#include <stdint.h>

SemaphoreHandle_t bin_sem;
SemaphoreHandle_t count_sem;
MutexHandle_t mtx;
MutexHandle_t rmtx;

int shared_counter = 0;

// Simple delay for demo purposes
void delay(volatile uint32_t count)
{
    while (count--)
        ;
}

//---------------------------- Task Functions ----------------------------//

// Task for testing binary semaphore
void task_bin_sem(void *arg)
{
    int id = *(int *)arg;
    for (int i = 0; i < 3; i++)
    {
        v_log(LOG_INFO, "Task %d: Waiting for binary semaphore...", id);
        if (v_semaphore_take(bin_sem, 1000) == VA_PASS)
        {
            v_log(LOG_INFO, "Task %d: Acquired binary semaphore!", id);
            shared_counter++;
            delay(50000); // simulate work
            v_semaphore_give(bin_sem);
            v_log(LOG_INFO, "Task %d: Released binary semaphore.", id);
        }
        else
        {
            v_log(LOG_WARN, "Task %d: Timeout waiting for binary semaphore!", id);
        }
    }
}

// Task for testing counting semaphore
void task_count_sem(void *arg)
{
    int id = *(int *)arg;
    for (int i = 0; i < 3; i++)
    {
        v_log(LOG_INFO, "Task %d: Waiting for counting semaphore...", id);
        if (v_semaphore_take(count_sem, 1000) == VA_PASS)
        {
            v_log(LOG_INFO, "Task %d: Acquired counting semaphore!", id);
            shared_counter++;
            delay(50000);
            v_semaphore_give(count_sem);
            v_log(LOG_INFO, "Task %d: Released counting semaphore.", id);
        }
        else
        {
            v_log(LOG_WARN, "Task %d: Timeout waiting for counting semaphore!", id);
        }
    }
}

// Task for testing mutex
void task_mutex(void *arg)
{
    int id = *(int *)arg;
    for (int i = 0; i < 3; i++)
    {
        v_log(LOG_INFO, "Task %d: Locking mutex...", id);
        if (v_mutex_lock(mtx, 1000) == VA_PASS)
        {
            v_log(LOG_INFO, "Task %d: Acquired mutex!", id);
            shared_counter++;
            delay(50000);
            v_mutex_unlock(mtx);
            v_log(LOG_INFO, "Task %d: Released mutex.", id);
        }
        else
        {
            v_log(LOG_WARN, "Task %d: Timeout locking mutex!", id);
        }
    }
}

// Task for testing recursive mutex
void task_recursive_mutex(void *arg)
{
    int id = *(int *)arg;
    v_log(LOG_INFO, "Task %d: Locking recursive mutex twice...", id);

    if (v_mutex_lock_recursive(rmtx, 1000) == VA_PASS)
    {
        v_log(LOG_INFO, "Task %d: First lock acquired.", id);
        if (v_mutex_lock_recursive(rmtx, 1000) == VA_PASS)
        {
            v_log(LOG_INFO, "Task %d: Second lock acquired.", id);
            shared_counter++;
            delay(50000);

            v_mutex_unlock_recursive(rmtx);
            v_log(LOG_INFO, "Task %d: First unlock done.", id);
            v_mutex_unlock_recursive(rmtx);
            v_log(LOG_INFO, "Task %d: Second unlock done.", id);
        }
    }
}

// Simulate an ISR giving a semaphore
void isr_simulator(void *arg)
{
    delay(200000); // let tasks block
    v_log(LOG_INFO, "ISR: Giving binary semaphore...");
    int woken = 0;
    v_semaphore_give_from_isr(bin_sem, &woken);
    if (woken)
        v_log(LOG_INFO, "ISR: Higher priority task was woken!");
}

//---------------------------- Kernel Task ----------------------------//

void kernel_task(void *arg)
{
    static int id1 = 1, id2 = 2, id3 = 3, id4 = 4;

    bin_sem = v_semaphore_create_binary();
    count_sem = v_semaphore_create_counting(2, 1);
    mtx = v_mutex_create();
    rmtx = v_mutex_create_recursive();

    task_create(task_bin_sem, &id1, 512, 0);
    task_create(task_bin_sem, &id2, 512, 0);
    task_create(task_count_sem, &id3, 512, 0);
    task_create(task_mutex, &id4, 512, 0);
    task_create(task_recursive_mutex, &id1, 512, 0); // reuse id

    // task_create(isr_simulator, NULL, 512, 1);

    while (1)
    {
        // kernel idle loop
    }
}

//---------------------------- Main ----------------------------//

int main(void)
{
    v_init();
    v_heap_memory_init();
    scheduler_init();

    task_create(kernel_task, NULL, 1024, 0);
    scheduler_start();

    while (1)
        ;
}
