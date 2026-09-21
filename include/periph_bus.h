#ifndef VAIOS_PERIPH_BUS_H
#define VAIOS_PERIPH_BUS_H

// Bus arbiter for a shared peripheral (I2C / SPI / UART). One v_pbus_t per
// physical bus; many users contend for it through a priority queue (FIFO within
// a priority). HAL-agnostic: a job's start() makes the actual hal_* call.
// Strict priority, no aging: a busy high-prio cyclic job can starve lower ones
// indefinitely; the only symptom is the starved job's cyclic overruns.
//
//   async  v_pbus_submit(): returns at once. When the job reaches the bus its
//          start() kicks a DMA transfer; the DMA-complete ISR must call
//          v_pbus_done_isr(), which runs done() and starts the next job
//          straight from the ISR (no context switch between back-to-back
//          transfers).
//   sync   v_pbus_lock()/v_pbus_unlock(): the caller queues like any job,
//          sleeps until granted, then does its blocking transfer in its own
//          task.
//   cyclic v_pbus_cyclic_add(): the job is re-submitted every `period` calls of
//          v_pbus_tick_isr(), which the board wires to a hardware timer IRQ.
//
// NavHAL DMA callbacks take no context, so each bus needs a trampoline:
//   static v_pbus_t i2c1;
//   static void i2c1_dma_done(void) { v_pbus_done_isr(&i2c1, 0); }
//   static void i2c1_timer(void)    { v_pbus_tick_isr(&i2c1); }
// A zero-initialised v_pbus_t is ready to use.
//
// Privileged callers only (kernel code, ISRs, privileged tasks): the critical
// sections are BASEPRI writes, which an unprivileged task's MSR silently skips,
// and there are no syscall wrappers for them. Unprivileged tasks use the
// fd-based transfer API at the bottom of this file instead.

#include "ipc.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct v_pbus_job v_pbus_job_t;
struct v_pbus_job {
  // --- set by the user ---
  // Start the transfer (typically a hal_*_dma call). Runs in whatever context
  // dispatched the job, often an ISR: never block. Return >= 0 once the DMA is
  // in flight, < 0 on failure (done() then gets that rc and the bus moves on).
  int (*start)(v_pbus_job_t *job);
  // Optional completion hook, usually in ISR context. May resubmit the job.
  void (*done)(v_pbus_job_t *job, int rc);
  void *arg;
  uint8_t prio;    // higher is served first
  uint32_t period; // cyclic only: v_pbus_tick_isr() calls between submissions
  // --- private ---
  v_pbus_job_t *next;
  v_pbus_job_t *cyc_next;
  SemaphoreHandle_t grant; // non-NULL only for a v_pbus_lock() waiter
  uint32_t countdown;
  volatile uint8_t state;
};

typedef struct {
  v_pbus_job_t *queue;        // pending, sorted by prio (desc), FIFO within
  v_pbus_job_t *active;       // async job on the bus, NULL otherwise
  volatile uint8_t locked;    // held by a v_pbus_lock() caller
  v_pbus_job_t *cyclic;       // registered cyclic jobs
  volatile uint32_t overruns; // cyclic ticks skipped: previous run still busy
} v_pbus_t;

// Queue a job (task or ISR context). VA_FAIL if it is already queued/active.
int v_pbus_submit(v_pbus_t *bus, v_pbus_job_t *job);
// DMA-complete hook: finish the active job with rc and dispatch the next one.
void v_pbus_done_isr(v_pbus_t *bus, int rc);
// Recovery for a wedged async transfer (slave holding SDA, lost DMA IRQ): fail
// the active job with rc and dispatch the next. The caller (a port watchdog)
// must first stop the DMA / reset the peripheral, so no late v_pbus_done_isr()
// can arrive and release the NEXT job. VA_FAIL if idle or held by v_pbus_lock.
int v_pbus_abort_isr(v_pbus_t *bus, int rc);

// Take the bus for a blocking transfer; VA_FAIL on timeout, or when more than
// VAIOS_PBUS_MAX_LOCKERS tasks are waiting on / holding bus locks at once (a
// waiter's queue node lives in that kernel pool, not on its stack). Task
// context only. Only the task that took the lock can release it.
int v_pbus_lock(v_pbus_t *bus, uint8_t prio, uint32_t ticks_to_wait);
void v_pbus_unlock(v_pbus_t *bus);
// Task-exit hook (kernel): drop t's queued lock requests and release any bus it
// holds, so a killed task leaves neither a dangling queue node nor a bus
// locked forever. Called alongside v_ipc_task_teardown.
struct Task_Control_Block;
void v_pbus_task_teardown(struct Task_Control_Block *t);

// Register / unregister a cyclic job (task context). Does not cancel an
// instance that is already queued; that one runs once more.
int v_pbus_cyclic_add(v_pbus_t *bus, v_pbus_job_t *job);
void v_pbus_cyclic_remove(v_pbus_t *bus, v_pbus_job_t *job);
// Hardware-timer hook. The timer IRQ must sit at a kernel-masked priority
// (numerically >= MAX_SYSCALL_INTERRUPT_PRIORITY), like any *_isr caller.
void v_pbus_tick_isr(v_pbus_t *bus);

// --- User access (VAIOS_DEVFS) ----------------------------------------------
// An unprivileged task can neither touch peripheral registers nor hand the
// kernel a callback, so it reaches a bus through a privileged per-bus transfer
// hook the board registers by name, and one blocking call per transfer:
//
//   board:  v_pbus_register("i2c1", &i2c1, i2c1_xfer_start);   // at boot
//   task:   int fd = v_pbus_open("i2c1");
//           v_pbus_xfer_t x = {.addr = 0x68, .tx = &reg, .tx_len = 1,
//                              .rx = buf, .rx_len = 6};
//           int rc = v_pbus_xfer(fd, &x, prio, ticks);          // close: v_file_close
//
// Payloads are copied through kernel buffers (VAIOS_PBUS_XFER_MAX each way), so
// the DMA never targets task memory: a task that times out or exits mid-
// transfer can't have the late DMA land in memory it no longer owns.
typedef struct {
  uint16_t addr;  // device address / chip select: meaning is up to the hook
  uint16_t flags; // board-defined
  const void *tx;
  uint32_t tx_len;
  void *rx;
  uint32_t rx_len;
} v_pbus_xfer_t;

// Board hook: start `x` (kernel buffers) on its bus. Same contract as
// v_pbus_job.start: never block; >= 0 once in flight, then v_pbus_done_isr().
typedef int (*v_pbus_xfer_fn)(const v_pbus_xfer_t *x);

#define V_PBUS_EINVAL (-22)
#define V_PBUS_EBUSY (-16)     // handle already has a transfer in flight
#define V_PBUS_ETIMEDOUT (-110) // timed out while still queued (never started)

#if VAIOS_DEVFS
// Privileged, at boot. `name` must outlive the registration.
int v_pbus_register(const char *name, v_pbus_t *bus, v_pbus_xfer_fn start);
// Open a registered bus -> fd, or a negative error.
int v_pbus_open(const char *name);
// Run one transfer: the hook's rc (>= 0 ok), a hook error, or V_PBUS_E*. If
// `ticks` expire after the transfer started, returns V_PBUS_EBUSY: its result
// is dropped and the fd takes new transfers once the bus finishes it.
int v_pbus_xfer(int fd, const v_pbus_xfer_t *x, uint8_t prio, uint32_t ticks);
// The three syscall steps v_pbus_xfer is built from.
int v_pbus_xfer_submit(int fd, const v_pbus_xfer_t *x, uint8_t prio);
int v_pbus_xfer_wait(int fd, uint32_t ticks);
int v_pbus_xfer_finish(int fd, void *rx, uint32_t rx_len);
#endif

#ifdef __cplusplus
}
#endif

#endif // VAIOS_PERIPH_BUS_H
