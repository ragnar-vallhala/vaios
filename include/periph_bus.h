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

// Take the bus for a blocking transfer; VA_FAIL on timeout. Task context only.
int v_pbus_lock(v_pbus_t *bus, uint8_t prio, uint32_t ticks_to_wait);
void v_pbus_unlock(v_pbus_t *bus);

// Register / unregister a cyclic job (task context). Does not cancel an
// instance that is already queued; that one runs once more.
int v_pbus_cyclic_add(v_pbus_t *bus, v_pbus_job_t *job);
void v_pbus_cyclic_remove(v_pbus_t *bus, v_pbus_job_t *job);
// Hardware-timer hook. The timer IRQ must sit at a kernel-masked priority
// (numerically >= MAX_SYSCALL_INTERRUPT_PRIORITY), like any *_isr caller.
void v_pbus_tick_isr(v_pbus_t *bus);

#ifdef __cplusplus
}
#endif

#endif // VAIOS_PERIPH_BUS_H
