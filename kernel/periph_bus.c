// Bus arbiter — see include/periph_bus.h for the model.
//
// All queue state is touched under ENTER_CRITICAL_FROM_ISR: every entry point
// may run in a task or in an ISR, and the FROM_ISR variant saves/restores
// BASEPRI locally instead of bumping the task nesting counter.
#include "periph_bus.h"
#include "port.h" // ENTER/EXIT_CRITICAL_FROM_ISR, v_port_trigger_pendsv

enum { JOB_IDLE = 0, JOB_QUEUED, JOB_ACTIVE };

// Caller holds the critical section.
static void enqueue(v_pbus_t *bus, v_pbus_job_t *job) {
  v_pbus_job_t **pp = &bus->queue;
  while (*pp && (*pp)->prio >= job->prio)
    pp = &(*pp)->next;
  job->next = *pp;
  *pp = job;
  job->state = JOB_QUEUED;
}

// Give the bus up; runs the owner's done() outside the critical section.
static void release(v_pbus_t *bus, int rc) {
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  v_pbus_job_t *job = bus->active;
  bus->active = NULL;
  bus->locked = 0;
  if (job)
    job->state = JOB_IDLE; // before done(), so done() may resubmit
  EXIT_CRITICAL_FROM_ISR(s);
  if (job && job->done)
    job->done(job, rc);
}

// If the bus is idle, hand it to the head of the queue. Loops only past async
// jobs whose start() failed; a started DMA continues via v_pbus_done_isr().
static void dispatch(v_pbus_t *bus) {
  for (;;) {
    int woken = 0;
    uint32_t s = ENTER_CRITICAL_FROM_ISR();
    v_pbus_job_t *job = (bus->active || bus->locked) ? NULL : bus->queue;
    if (job) {
      bus->queue = job->next;
      job->state = JOB_ACTIVE;
      // A lock waiter's job lives in v_pbus_lock's stack frame, gone once it
      // returns, so the bus records only that it is locked.
      if (job->grant)
        bus->locked = 1;
      else
        bus->active = job;
      // Give inside the critical section: a timed-out v_pbus_lock() must not
      // see ACTIVE and return (killing its stack semaphore) before this lands.
      if (job->grant)
        v_semaphore_give_from_isr(job->grant, &woken);
    }
    EXIT_CRITICAL_FROM_ISR(s);
    if (!job)
      return;
    if (job->grant) {
      if (woken)
        v_port_trigger_pendsv();
      return;
    }
    int rc = job->start(job);
    if (rc >= 0)
      return;
    release(bus, rc);
  }
}

int v_pbus_submit(v_pbus_t *bus, v_pbus_job_t *job) {
  if (!bus || !job || !job->start)
    return VA_FAIL;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  if (job->state != JOB_IDLE) {
    EXIT_CRITICAL_FROM_ISR(s);
    return VA_FAIL;
  }
  job->grant = NULL;
  enqueue(bus, job);
  EXIT_CRITICAL_FROM_ISR(s);
  dispatch(bus);
  return VA_PASS;
}

void v_pbus_done_isr(v_pbus_t *bus, int rc) {
  release(bus, rc);
  dispatch(bus);
}

int v_pbus_abort_isr(v_pbus_t *bus, int rc) {
  if (!bus || !bus->active)
    return VA_FAIL; // idle, or held by v_pbus_lock (its HAL call times out)
  release(bus, rc);
  dispatch(bus);
  return VA_PASS;
}

int v_pbus_lock(v_pbus_t *bus, uint8_t prio, uint32_t ticks_to_wait) {
  if (!bus)
    return VA_FAIL;
  StaticSemaphore_t sem_buf;
  v_pbus_job_t job = {.prio = prio};
  job.grant = v_semaphore_create_binary_static(&sem_buf);

  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  enqueue(bus, &job);
  EXIT_CRITICAL_FROM_ISR(s);
  dispatch(bus);

  if (v_semaphore_take(job.grant, ticks_to_wait) == VA_PASS)
    return VA_PASS;

  // Timed out — unless the grant raced in after the take gave up.
  s = ENTER_CRITICAL_FROM_ISR();
  int granted = job.state == JOB_ACTIVE;
  if (!granted) {
    v_pbus_job_t **pp = &bus->queue;
    while (*pp && *pp != &job)
      pp = &(*pp)->next;
    if (*pp)
      *pp = job.next;
  }
  EXIT_CRITICAL_FROM_ISR(s);
  return granted ? VA_PASS : VA_FAIL;
}

void v_pbus_unlock(v_pbus_t *bus) {
  if (!bus || !bus->locked)
    return;
  release(bus, 0);
  dispatch(bus);
}

int v_pbus_cyclic_add(v_pbus_t *bus, v_pbus_job_t *job) {
  if (!bus || !job || !job->start || !job->period)
    return VA_FAIL;
  job->countdown = job->period;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  job->cyc_next = bus->cyclic;
  bus->cyclic = job;
  EXIT_CRITICAL_FROM_ISR(s);
  return VA_PASS;
}

void v_pbus_cyclic_remove(v_pbus_t *bus, v_pbus_job_t *job) {
  if (!bus || !job)
    return;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  for (v_pbus_job_t **pp = &bus->cyclic; *pp; pp = &(*pp)->cyc_next) {
    if (*pp == job) {
      *pp = job->cyc_next;
      break;
    }
  }
  EXIT_CRITICAL_FROM_ISR(s);
}

// ponytail: O(cyclic jobs) countdown per tick at one base rate; switch to a
// compare-match one-shot on the nearest deadline if the list grows or jitter
// below one timer period matters.
// Walks bus->cyclic without a critical section on purpose: add/remove splice a
// single link under ENTER_CRITICAL_FROM_ISR, which masks this (kernel-priority)
// IRQ, and a task cannot preempt an ISR, so the walk never sees a half-splice.
void v_pbus_tick_isr(v_pbus_t *bus) {
  for (v_pbus_job_t *job = bus->cyclic; job; job = job->cyc_next) {
    if (--job->countdown)
      continue;
    job->countdown = job->period;
    if (v_pbus_submit(bus, job) != VA_PASS)
      bus->overruns++;
  }
}
