/*
 * 59 — an UNPRIVILEGED task using the SD card through the VFS mount (roadmap
 * M5's on-target gate, and the measurement that settles decision D3).
 *
 * Renode's generic STM32F4 has no SD card, so this one is hardware-only:
 * tools/run_hw_tests.sh, or flash it and read USART2.
 *
 * Privileged init mounts the VFS as a devfs node; after that the file work is
 * done by a task that cannot touch the filesystem's state, its mutex, or the
 * SDIO peripheral — only the fd syscalls.
 *
 *   vfsio (PRIVILEGED, low priority) runs v_vfs_worker_step: the task that
 *                                   actually touches the card, in thread mode
 *                                   where interrupts are enabled. Without it a
 *                                   task's file calls fail with -11 rather than
 *                                   hanging.
 *   logger (unpriv, low priority)   writes RECORDS records, syncs, closes,
 *                                   reopens, reads them back and verifies,
 *                                   stats the file, makes a directory, lists
 *                                   it, unlinks, and checks that a kernel
 *                                   pointer is refused with -14 rather than
 *                                   faulting.
 *   controller (unpriv, HIGH prio)  holds a 5 ms cadence with task_delay_until
 *                                   throughout, and reports its WORST drift.
 *
 * The controller is the point of the test. Its worst drift says what file I/O
 * costs a task that wants nothing to do with the filesystem. The first version
 * of this example ran the filesystem inside the syscall and DEADLOCKED on
 * hardware — SVCall is priority 0, so the handler masked the SDIO completion it
 * was waiting for, and the tick with it. Hence the worker: with the transfers in
 * thread mode, the controller should keep its cadence.
 *
 * Paths: the mount prefix is a devfs namespace and the rest is handed to the
 * filesystem verbatim, so FatFs drive syntax applies — "/mnt/0:name".
 */
#ifndef NAVHAL
#error "NAVHAL is required for this example"
#endif
#include "navhal.h"
#include "port.h" // v_port_is_privileged
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include "vfile.h"
#include "ipc.h" // VA_PASS
#include "vfs.h"

#define IO_TICKS 500 // how long a file call waits for the worker

#define DATA_FILE "/mnt/0:sduser.dat"
#define DATA_DIR "/mnt/0:vlogs"
#define LIST_DIR "/mnt/0:"
#define RECORDS 48
#define RECORD_SZ 64
#define CTL_PERIOD_TICKS 5
#define CTL_CYCLES 500 // 2.5 s, comfortably longer than the file work
#define KERNEL_SRAM ((void *)0x20000010u)
#define EFAULT_RC (-14)

static void say(const char *fmt, int a, int b) {
  char buf[96];
  int n = print_fmt_buf(buf, sizeof buf, fmt, a, b);
  v_file_write(1, buf, n);
}

// A record whose contents depend on its index, so a read-back mismatch is
// detectable without keeping the whole file in RAM.
static void fill_record(uint8_t *rec, uint32_t i) {
  for (uint32_t k = 0; k < RECORD_SZ; k++)
    rec[k] = (uint8_t)(i * 7u + k);
}

// The filesystem's own task. It must be PRIVILEGED: it touches the request
// slots, the FatFs state and the SDIO peripheral. Thread mode with interrupts
// enabled is the point — that is where a transfer can actually complete. Priority
// 1, below the controller, because recording must never outrank control.
static void vfsio_task(void *arg) {
  (void)arg;
  for (;;)
    v_vfs_worker_step(100);
}

static void logger_task(void *arg) {
  (void)arg;
  int bad = 0;
  uint8_t rec[RECORD_SZ];
  say("[sduser] logger nPRIV=%d\r\n", v_port_is_privileged() ? 0 : 1, 0);

  // --- write ---------------------------------------------------------------
  int fd = v_vfs_open(DATA_FILE, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC, IO_TICKS);
  if (fd < 0) {
    say("[sduser] open for write failed %d\r\n", fd, 0);
    bad = 1;
  }
  for (uint32_t i = 0; i < RECORDS && !bad; i++) {
    fill_record(rec, i);
    int n = v_vfs_write(fd, rec, RECORD_SZ, IO_TICKS);
    if (n != RECORD_SZ) {
      say("[sduser] write %d returned %d\r\n", (int)i, n);
      bad = 1;
    }
    if ((i & 15u) == 15u && v_vfs_flush(fd, IO_TICKS) != 0) {
      say("[sduser] sync failed at %d\r\n", (int)i, 0);
      bad = 1;
    }
  }
  // A kernel pointer as the source is refused, not faulted on.
  if (!bad) {
    int rc = v_vfs_write(fd, KERNEL_SRAM, 4, IO_TICKS);
    if (rc != EFAULT_RC) {
      say("[sduser] kernel src rc=%d (want %d)\r\n", rc, EFAULT_RC);
      bad = 1;
    }
  }
  if (fd >= 0)
    v_vfs_close(fd, IO_TICKS);

  // --- read back -----------------------------------------------------------
  if (!bad) {
    fd = v_vfs_open(DATA_FILE, VFS_O_RDONLY, IO_TICKS);
    if (fd < 0) {
      say("[sduser] open for read failed %d\r\n", fd, 0);
      bad = 1;
    }
    for (uint32_t i = 0; i < RECORDS && !bad; i++) {
      uint8_t got[RECORD_SZ], want[RECORD_SZ];
      int n = v_vfs_read(fd, got, RECORD_SZ, IO_TICKS);
      if (n != RECORD_SZ) {
        say("[sduser] read %d returned %d\r\n", (int)i, n);
        bad = 1;
        break;
      }
      fill_record(want, i);
      for (uint32_t k = 0; k < RECORD_SZ; k++)
        if (got[k] != want[k]) {
          say("[sduser] record %d byte %d wrong\r\n", (int)i, (int)k);
          bad = 1;
          break;
        }
    }
    // Seek back and re-read the first record: lseek is its own syscall.
    if (!bad) {
      if (v_vfs_seek(fd, 0, 0, IO_TICKS) != 0) {
        say("[sduser] lseek failed\r\n", 0, 0);
        bad = 1;
      } else {
        uint8_t got[RECORD_SZ], want[RECORD_SZ];
        v_vfs_read(fd, got, RECORD_SZ, IO_TICKS);
        fill_record(want, 0);
        for (uint32_t k = 0; k < RECORD_SZ; k++)
          if (got[k] != want[k])
            bad = 1;
        if (bad)
          say("[sduser] re-read after lseek wrong\r\n", 0, 0);
      }
    }
    if (fd >= 0)
      v_vfs_close(fd, IO_TICKS);
  }

  // --- stat, mkdir, list, unlink ------------------------------------------
  if (!bad) {
    vfs_stat_t st;
    if (v_vfs_info(DATA_FILE, &st, IO_TICKS) != 0 ||
        st.size != (uint32_t)RECORDS * RECORD_SZ) {
      say("[sduser] stat size=%d want=%d\r\n", (int)st.size,
          RECORDS * RECORD_SZ);
      bad = 1;
    }
  }
  if (!bad) {
    v_vfs_makedir(DATA_DIR, IO_TICKS); // may already exist from an earlier run
    int dir = v_vfs_diropen(LIST_DIR, IO_TICKS);
    if (dir < 0) {
      say("[sduser] opendir failed %d\r\n", dir, 0);
      bad = 1;
    } else {
      int entries = 0, found = 0;
      vfs_dirent_t ent;
      while (v_vfs_dirnext(dir, &ent, IO_TICKS) == 1) {
        entries++;
        if (v_strcmp(ent.name, "SDUSER.DAT") == 0 ||
            v_strcmp(ent.name, "sduser.dat") == 0)
          found = 1;
      }
      v_vfs_close(dir, IO_TICKS);
      if (!entries || !found) {
        say("[sduser] listing entries=%d found=%d\r\n", entries, found);
        bad = 1;
      }
    }
  }
  if (!bad && v_vfs_remove(DATA_FILE, IO_TICKS) != 0) {
    say("[sduser] unlink failed\r\n", 0, 0);
    bad = 1;
  }
  if (!bad) { // and it really is gone
    vfs_stat_t st;
    if (v_vfs_info(DATA_FILE, &st, IO_TICKS) == 0) {
      say("[sduser] file survived unlink\r\n", 0, 0);
      bad = 1;
    }
  }

  say(bad ? "[sduser] logger FAIL\r\n" : "[sduser] logger PASS\r\n", 0, 0);
  for (;;)
    v_delay(1000);
}

// The higher-priority task that wants nothing to do with the SD card. Its worst
// drift is the answer to "is a synchronous VFS syscall good enough?".
static void controller_task(void *arg) {
  (void)arg;
  say("[sduser] controller nPRIV=%d\r\n", v_port_is_privileged() ? 0 : 1, 0);
  uint32_t next = v_get_ticks(), worst = 0, late = 0;
  for (uint32_t i = 0; i < CTL_CYCLES; i++) {
    uint32_t want = next + CTL_PERIOD_TICKS;
    task_delay_until(&next, CTL_PERIOD_TICKS);
    uint32_t now = v_get_ticks();
    uint32_t drift = now > want ? now - want : 0; // woke late by this much
    if (drift > worst)
      worst = drift;
    if (drift > 1u)
      late++;
  }
  say("[sduser] cadence worst_drift=%d ticks, late_cycles=%d\r\n", (int)worst,
      (int)late);
  // A tick or two is the scheduler's own granularity; tens of ticks would mean
  // the filesystem held the kernel and D3 needs the I/O-worker answer.
  say(worst <= 3u ? "[sduser] controller PASS\r\n"
                  : "[sduser] controller FAIL\r\n",
      0, 0);
  for (;;)
    v_delay(1000);
}

int main(void) {
  // Clocks, UART, SysTick, heap, scheduler, SDIO and the VFS.
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 1};
  v_system_init(&cfg);

  if (v_vfs_mount("/mnt/") != VA_PASS) {
    v_log(LOG_ERROR, "sd_user: mount failed");
    while (1)
      ;
  }
  v_log(LOG_INFO, "sd_user: start (privileged init only)");

  task_create_privileged(vfsio_task, NULL, 2048, 1, "vfsio");
  task_create_named(controller_task, NULL, 2048, 3, "controller");
  task_create_named(logger_task, NULL, 2048, 1, "logger");
  scheduler_start();
  for (;;)
    ;
}
