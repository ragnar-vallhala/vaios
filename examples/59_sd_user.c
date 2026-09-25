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
 *   logger (unpriv, low priority)   writes RECORDS records, syncs, closes,
 *                                   reopens, reads them back and verifies,
 *                                   stats the file, makes a directory, lists
 *                                   it, unlinks, and checks that a kernel
 *                                   pointer is refused with -14 rather than
 *                                   faulting.
 *   controller (unpriv, HIGH prio)  holds a 5 ms cadence with task_delay_until
 *                                   throughout, and reports its WORST drift.
 *
 * The second task is the point of the test. An SD write takes milliseconds, and
 * the VFS syscall is synchronous: the caller blocks inside the kernel on the
 * FatFs mutex. The open question is whether that delays a higher-priority task
 * that wants nothing to do with the filesystem. If max_drift stays at a tick or
 * two, synchronous is fine and D3 is settled; if it is tens of ticks, the VFS
 * needs an I/O worker task and the syscall should queue to it.
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

static void logger_task(void *arg) {
  (void)arg;
  int bad = 0;
  uint8_t rec[RECORD_SZ];
  say("[sduser] logger nPRIV=%d\r\n", v_port_is_privileged() ? 0 : 1, 0);

  // --- write ---------------------------------------------------------------
  int fd = v_file_open(DATA_FILE, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
  if (fd < 0) {
    say("[sduser] open for write failed %d\r\n", fd, 0);
    bad = 1;
  }
  for (uint32_t i = 0; i < RECORDS && !bad; i++) {
    fill_record(rec, i);
    int n = v_file_write(fd, rec, RECORD_SZ);
    if (n != RECORD_SZ) {
      say("[sduser] write %d returned %d\r\n", (int)i, n);
      bad = 1;
    }
    if ((i & 15u) == 15u && v_file_sync(fd) != 0) {
      say("[sduser] sync failed at %d\r\n", (int)i, 0);
      bad = 1;
    }
  }
  // A kernel pointer as the source is refused, not faulted on.
  if (!bad) {
    int rc = v_file_write(fd, KERNEL_SRAM, 4);
    if (rc != EFAULT_RC) {
      say("[sduser] kernel src rc=%d (want %d)\r\n", rc, EFAULT_RC);
      bad = 1;
    }
  }
  if (fd >= 0)
    v_file_close(fd);

  // --- read back -----------------------------------------------------------
  if (!bad) {
    fd = v_file_open(DATA_FILE, VFS_O_RDONLY);
    if (fd < 0) {
      say("[sduser] open for read failed %d\r\n", fd, 0);
      bad = 1;
    }
    for (uint32_t i = 0; i < RECORDS && !bad; i++) {
      uint8_t got[RECORD_SZ], want[RECORD_SZ];
      int n = v_file_read(fd, got, RECORD_SZ);
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
      if (v_file_lseek(fd, 0, 0) != 0) {
        say("[sduser] lseek failed\r\n", 0, 0);
        bad = 1;
      } else {
        uint8_t got[RECORD_SZ], want[RECORD_SZ];
        v_file_read(fd, got, RECORD_SZ);
        fill_record(want, 0);
        for (uint32_t k = 0; k < RECORD_SZ; k++)
          if (got[k] != want[k])
            bad = 1;
        if (bad)
          say("[sduser] re-read after lseek wrong\r\n", 0, 0);
      }
    }
    if (fd >= 0)
      v_file_close(fd);
  }

  // --- stat, mkdir, list, unlink ------------------------------------------
  if (!bad) {
    vfs_stat_t st;
    if (v_file_stat(DATA_FILE, &st) != 0 ||
        st.size != (uint32_t)RECORDS * RECORD_SZ) {
      say("[sduser] stat size=%d want=%d\r\n", (int)st.size,
          RECORDS * RECORD_SZ);
      bad = 1;
    }
  }
  if (!bad) {
    v_file_mkdir(DATA_DIR); // may already exist from an earlier run
    int dir = v_dir_open(LIST_DIR);
    if (dir < 0) {
      say("[sduser] opendir failed %d\r\n", dir, 0);
      bad = 1;
    } else {
      int entries = 0, found = 0;
      vfs_dirent_t ent;
      while (v_dir_read(dir, &ent) == 1) {
        entries++;
        if (v_strcmp(ent.name, "SDUSER.DAT") == 0 ||
            v_strcmp(ent.name, "sduser.dat") == 0)
          found = 1;
      }
      v_file_close(dir);
      if (!entries || !found) {
        say("[sduser] listing entries=%d found=%d\r\n", entries, found);
        bad = 1;
      }
    }
  }
  if (!bad && v_file_unlink(DATA_FILE) != 0) {
    say("[sduser] unlink failed\r\n", 0, 0);
    bad = 1;
  }
  if (!bad) { // and it really is gone
    vfs_stat_t st;
    if (v_file_stat(DATA_FILE, &st) == 0) {
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

  task_create_named(controller_task, NULL, 2048, 3, "controller");
  task_create_named(logger_task, NULL, 2048, 1, "logger");
  scheduler_start();
  for (;;)
    ;
}
