#include "navhal.h"
#include "task.h"
#include "vaios.h"
#include "vfs.h"
#include <stdint.h>

/* File config */
#define LOG_FILE "0:single.dat"
#define LOG_FILE_SIZE (512 * 64U) /* 32 KB */
#define TEST_DATA "Hello Single Task!\n"
#define TEST_DATA_LEN (sizeof(TEST_DATA) - 1)

/* Write cursor */
static uint32_t write_pos = 0;

/* ----------------------------------------------------------
 * Single writer task
 * ---------------------------------------------------------- */
void writer_task(void *arg) {
  (void)arg;

  while (1) {
    vfs_fd_t fd = vfs_open(LOG_FILE, VFS_O_RDWR | VFS_O_CREAT);

    if (fd >= 0) {
      /* Seek to circular position */
      vfs_lseek(fd, (long)write_pos, VFS_SEEK_SET);

      int written = vfs_write(fd, TEST_DATA, TEST_DATA_LEN);

      vfs_sync(fd);
      vfs_close(fd);

      hal_uart_print(HAL_UART_2, "written: ");
      hal_uart_print(HAL_UART_2, written);
      hal_uart_print(HAL_UART_2, "\n\r");

      /* Advance circular buffer */
      write_pos = (write_pos + TEST_DATA_LEN) % (LOG_FILE_SIZE - TEST_DATA_LEN);

    } else {
      hal_uart_print(HAL_UART_2, "open failed\n\r");
    }

    v_delay(1000); // safe delay
  }
}

/* ----------------------------------------------------------
 * Main
 * ---------------------------------------------------------- */
int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 1};

  v_system_init(&cfg);

  hal_uart_print(HAL_UART_2, "System Init Done\n\r");

  /* Preallocate ONCE */
  int pa_ret = vfs_preallocate(LOG_FILE, LOG_FILE_SIZE);

  hal_uart_print(HAL_UART_2, "prealloc: ");
  hal_uart_print(HAL_UART_2, pa_ret);
  hal_uart_print(HAL_UART_2, "\n\r");

  if (pa_ret != 0) {
    hal_uart_print(HAL_UART_2, "Prealloc failed\n\r");
    while (1)
      ;
  }

  /* Create ONLY ONE task */
  task_create(writer_task, NULL, 1024, 1);
  task_create(writer_task, NULL, 1024, 1);

  scheduler_start();

  while (1)
    ;
}