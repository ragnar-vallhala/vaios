/**
 * @file vfs_concurrent.c
 * @brief VFS concurrent read/write example using pre-allocated file.
 *
 * A fixed-size log file is pre-allocated once at boot using f_expand().
 * All subsequent writes seek to an in-place position instead of appending,
 * so the FAT cluster chain is NEVER modified after the first boot —
 * making the filesystem nearly crash-safe against hard resets.
 */
#include "memory.h"
#include "navhal.h"
#include "port.h"
#include "utils.h"
#include "vaios.h"
#include "vfs.h"
#include <stddef.h>
#include <stdint.h>
#include <task.h>

/* Log file configuration */
#define LOG_FILE "0:vaioslog.dat"
#define LOG_FILE_SIZE (512 * 64U) /* 64 sectors = 32 KB pre-allocated */
#define TEST_DATA "Hello from Vaios VFS!"
#define TEST_DATA_LEN (sizeof(TEST_DATA) - 1)

/* Current write cursor — shared between writer (write) and reader (read only).
 * Updated by the writer AFTER close, so the reader always sees a stable value.
 */
static volatile uint32_t write_pos = 0;

/* ------------------------------------------------------------------
 * Writer: seek to write_pos, write in-place, sync, close, advance.
 * The FAT chain is already allocated — no FAT update during the write.
 * ------------------------------------------------------------------ */
static volatile uint8_t started = 0;
void writer_task(void *arg) {
  while (1) {
    v_log(LOG_INFO, "[Writer] Opening file for in-place write at pos %d...",
          (int)write_pos);

    vfs_fd_t fd = vfs_open(LOG_FILE, VFS_O_RDWR | VFS_O_CREAT);
    if (fd >= 0) {
      /* Seek to the circular write position */
      vfs_lseek(fd, (long)write_pos, VFS_SEEK_SET);
      int written = vfs_write(fd, TEST_DATA, TEST_DATA_LEN);
      vfs_sync(fd);
      vfs_close(fd);
      started = 1;
      v_log(LOG_INFO, "[Writer] Wrote %d bytes.", written);

      /* Advance write cursor circularly — only AFTER close so reader is safe */
      write_pos = (write_pos + TEST_DATA_LEN) % (LOG_FILE_SIZE - TEST_DATA_LEN);
    } else {
      v_log(LOG_ERROR, "[Writer] Failed to open file (code: %d).", fd);
    }

    for (volatile int i = 0; i < 1000000; i++)
      ;
    task_yield();
  }
}

/* ------------------------------------------------------------------
 * Reader: seek to the last write position and read back the written data.
 * ------------------------------------------------------------------ */
void reader_task(void *arg) {
  char read_buf[32];
  while (1) {
    if (!started) {
      task_yield();
      continue;
    }
    /* Compute read position: the slot just before the current write cursor.
     * Adding LOG_FILE_SIZE before subtracting handles the wrap-around case. */
    uint32_t read_pos =
        (write_pos + LOG_FILE_SIZE - TEST_DATA_LEN) % LOG_FILE_SIZE;

    v_log(LOG_INFO, "[Reader] Opening file for reading at pos %d...",
          (int)read_pos);

    vfs_fd_t fd = vfs_open(LOG_FILE, VFS_O_RDONLY);
    if (fd >= 0) {
      vfs_lseek(fd, (long)read_pos, VFS_SEEK_SET);
      v_memset(read_buf, 0, sizeof(read_buf));
      int read_bytes = vfs_read(fd, read_buf, TEST_DATA_LEN);
      vfs_close(fd);

      if (read_bytes == (int)TEST_DATA_LEN &&
          v_strncmp(read_buf, TEST_DATA, TEST_DATA_LEN) == 0) {
        v_log(LOG_INFO, "[Reader] Validation SUCCESS. Read: %s", read_buf);
      } else {
        v_log(LOG_ERROR, "[Reader] Validation FAILED. Read %d bytes.",
              read_bytes);
      }
    } else {
      v_log(LOG_ERROR, "[Reader] Failed to open file (code: %d).", fd);
    }

    for (volatile int i = 0; i < 1500000; i++)
      ;
    task_yield();
  }
}

int main(void) {
  /* Initialize entire system: Clock, UART, SysTick, Heap, Scheduler, SDIO, VFS
   */
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 1};
  v_system_init(&cfg);

  hal_uart_write_string(HAL_UART_2, "[MAIN] v_system_init done\r\n");

  /* Pre-allocate the log file once. On subsequent boots this is a no-op. */
  hal_uart_write_string(HAL_UART_2, "[MAIN] calling vfs_preallocate...\r\n");
  int pa_ret = vfs_preallocate(LOG_FILE, LOG_FILE_SIZE);
  hal_uart_write_string(HAL_UART_2, "[MAIN] vfs_preallocate returned: ");
  hal_uart_write_int(HAL_UART_2, pa_ret);
  hal_uart_write_string(HAL_UART_2, "\r\n");

  if (pa_ret != 0) {
    v_log(LOG_ERROR, "Failed to pre-allocate log file! (code %d)", pa_ret);
    while (1)
      ;
  }
  v_log(LOG_INFO, "Log file pre-allocated (%d bytes).", LOG_FILE_SIZE);

  /* Create Writer and Reader tasks */
  task_create(writer_task, NULL, 1024, 1);
  task_create(reader_task, NULL, 1024, 1);

  v_log(LOG_INFO, "Starting VFS Concurrent Example...");
  scheduler_start();

  while (1)
    ;
}
