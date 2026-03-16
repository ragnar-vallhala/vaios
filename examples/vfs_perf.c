#include "navhal.h"
#include "utils.h"
#include "vaios.h"
#include "vfs.h"
#include <stdint.h>
#include <task.h>

#define PERF_FILE "0:perf.bin"
#define BUFFER_SIZE (2*1024)     /* 16KB buffer */
#define TEST_SIZE (1 * 1024 * 1024) /* 1MB test file */

static uint8_t data_buf[BUFFER_SIZE] __attribute__((aligned(4)));

void perf_task(void *arg) {
  (void)arg;
  v_log(LOG_INFO, "VFS Performance Benchmark Started");
  v_log(LOG_INFO, "Buffer: %dKB, Total: %dKB", BUFFER_SIZE / 1024,
        TEST_SIZE / 1024);
  v_log_flush();

  /* Initialize buffer */
  for (int i = 0; i < BUFFER_SIZE; i++)
    data_buf[i] = (uint8_t)i;

  /* --- PRE-ALLOCATE --- */
  v_log(LOG_INFO, "Ensuring file allocation...");
  v_log_flush();
  int pre_res = vfs_preallocate(PERF_FILE, TEST_SIZE);
  if (pre_res < 0) {
    v_log(LOG_ERROR, "Pre-allocation failed: %d", pre_res);
    while (1)
      task_yield();
  }

  /* --- WRITE TEST --- */
  v_log(LOG_INFO, "Running WRITE test...");
  v_log_flush();

  vfs_fd_t fd = vfs_open(PERF_FILE, VFS_O_WRONLY);
  if (fd < 0) {
    v_log(LOG_ERROR, "Open failed: %d", fd);
    while (1)
      task_yield();
  }

  uint32_t start_ticks = v_get_ticks();
  uint32_t bytes_written = 0;
  while (bytes_written < TEST_SIZE) {
    int res = vfs_write(fd, data_buf, BUFFER_SIZE);
    if (res <= 0) {
      v_log(LOG_ERROR, "Write error at %u: %d", bytes_written, res);
      break;
    }
    bytes_written += res;
    /* Optional: yield every few blocks to keep system responsive */
    task_yield();
  }
  vfs_sync(fd);
  uint32_t end_ticks = v_get_ticks();
  vfs_close(fd);

  uint32_t duration = end_ticks - start_ticks;
  if (duration > 0) {
    // Use integer math to avoid FPU issues for now
    uint32_t kb_per_sec = (bytes_written / 1024) * 1000 / duration;
    v_log(LOG_INFO, "WRITE: %u bytes in %u ms (%u KB/s)", bytes_written,
          duration, kb_per_sec);
  }
  v_log_flush();

  /* --- READ TEST --- */
  v_log(LOG_INFO, "Running READ test...");
  v_log_flush();

  fd = vfs_open(PERF_FILE, VFS_O_RDONLY);
  if (fd < 0) {
    v_log(LOG_ERROR, "Open failed: %d", fd);
    while (1)
      task_yield();
  }

  start_ticks = v_get_ticks();
  uint32_t bytes_read = 0;
  while (bytes_read < TEST_SIZE) {
    int res = vfs_read(fd, data_buf, BUFFER_SIZE);
    if (res <= 0)
      break;
    bytes_read += res;
    task_yield();
  }
  end_ticks = v_get_ticks();
  vfs_close(fd);

  duration = end_ticks - start_ticks;
  if (duration > 0) {
    uint32_t kb_per_sec = (bytes_read / 1024) * 1000 / duration;
    v_log(LOG_INFO, "READ: %u bytes in %u ms (%u KB/s)", bytes_read, duration,
          kb_per_sec);
  }
  v_log_flush();

  v_log(LOG_INFO, "Benchmark Complete.");
  while (1)
    task_yield();
}

int main(void) {
  v_system_init();
  uart2_write_string("VAIOS Boot Successful");
  
  /* 8KB stack for safety */
  task_create(perf_task, NULL, 8192, 1);

  scheduler_start();
  while (1)
    ;
}
