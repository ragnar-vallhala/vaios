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

// Caller holds the critical section. Drop a still-queued job; 0 if not queued.
static int unqueue(v_pbus_t *bus, v_pbus_job_t *job) {
  v_pbus_job_t **pp = &bus->queue;
  while (*pp && *pp != job)
    pp = &(*pp)->next;
  if (!*pp)
    return 0;
  *pp = job->next;
  job->state = JOB_IDLE;
  return 1;
}

// Finish the active async job; runs its done() outside the critical section.
// Returns 0 if none was active (a stray/late DMA-complete, or the bus is held
// by v_pbus_lock): only v_pbus_unlock may clear a sync lock.
static int release(v_pbus_t *bus, int rc) {
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  v_pbus_job_t *job = bus->active;
  bus->active = NULL;
  if (job)
    job->state = JOB_IDLE; // before done(), so done() may resubmit
  EXIT_CRITICAL_FROM_ISR(s);
  if (!job)
    return 0;
  if (job->done)
    job->done(job, rc);
  return 1;
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
  // Checked inside release()'s critical section, not before it, so a DMA
  // IRQ landing in between can't make us release the NEXT owner.
  if (!bus || !release(bus, rc))
    return VA_FAIL; // idle, or held by v_pbus_lock (its HAL call times out)
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
  if (!granted)
    unqueue(bus, &job);
  EXIT_CRITICAL_FROM_ISR(s);
  return granted ? VA_PASS : VA_FAIL;
}

void v_pbus_unlock(v_pbus_t *bus) {
  if (!bus || !bus->locked)
    return;
  bus->locked = 0; // only the holder writes it while set; no race to guard
  dispatch(bus);
}

int v_pbus_cyclic_add(v_pbus_t *bus, v_pbus_job_t *job) {
  if (!bus || !job || !job->start || !job->period)
    return VA_FAIL;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  // A second add would self-link the list and spin v_pbus_tick_isr forever.
  for (v_pbus_job_t *j = bus->cyclic; j; j = j->cyc_next) {
    if (j == job) {
      EXIT_CRITICAL_FROM_ISR(s);
      return VA_FAIL;
    }
  }
  job->countdown = job->period;
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

// --- User access (VAIOS_DEVFS) ----------------------------------------------
// Each open handle owns a kernel job + bounce buffers, so nothing on the queue
// or under DMA lives in task memory. Bodies run privileged: reached from the
// syscall dispatch in handler mode, or directly when SVC is off.
#if VAIOS_DEVFS
#include "syscall.h"
#include "utils.h" // v_memcpy
#include "vfile.h"

#define PBUS_MAX_DEVS 4

typedef struct {
  const char *name;
  v_pbus_t *bus;
  v_pbus_xfer_fn start;
} pbus_dev_t;

typedef struct {
  pbus_dev_t *dev;
  v_pbus_job_t job;
  v_pbus_xfer_t x; // kernel copy; tx/rx point at the buffers below
  uint8_t tx[VAIOS_PBUS_XFER_MAX];
  uint8_t rx[VAIOS_PBUS_XFER_MAX];
  StaticSemaphore_t done_buf;
  SemaphoreHandle_t done;
  volatile int rc;
  uint8_t used;
  uint8_t submitted; // a transfer awaits v_pbus_xfer_finish
  uint8_t orphan;    // closed mid-transfer: done() frees the slot
} pbus_handle_t;

static pbus_dev_t pbus_devs[PBUS_MAX_DEVS];
static pbus_handle_t pbus_handles[VAIOS_PBUS_MAX_OPEN];

static int name_eq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

static int handle_start(v_pbus_job_t *job) {
  pbus_handle_t *h = (pbus_handle_t *)job->arg;
  return h->dev->start(&h->x);
}

// Completion, usually in the DMA ISR.
static void handle_done(v_pbus_job_t *job, int rc) {
  pbus_handle_t *h = (pbus_handle_t *)job->arg;
  h->rc = rc;
  if (h->orphan) {
    h->orphan = 0;
    h->used = 0;
    return;
  }
  int woken = 0;
  v_semaphore_give_from_isr(h->done, &woken);
  if (woken)
    v_port_trigger_pendsv();
}

// fd close, or task exit (v_fd_close_all). A queued transfer is dropped; one
// already on the bus keeps the slot until its done() lands.
static int handle_close(void *priv) {
  pbus_handle_t *h = (pbus_handle_t *)priv;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  if (h->job.state == JOB_QUEUED)
    unqueue(h->dev->bus, &h->job);
  h->submitted = 0;
  if (h->job.state == JOB_ACTIVE)
    h->orphan = 1;
  else
    h->used = 0;
  EXIT_CRITICAL_FROM_ISR(s);
  return 0;
}

static const v_file_ops pbus_ops = {
    .read = NULL, .write = NULL, .close = handle_close};

int v_pbus_register(const char *name, v_pbus_t *bus, v_pbus_xfer_fn start) {
  if (!name || !bus || !start)
    return V_PBUS_EINVAL;
  for (int i = 0; i < PBUS_MAX_DEVS; i++) {
    if (!pbus_devs[i].name) {
      pbus_devs[i] = (pbus_dev_t){.name = name, .bus = bus, .start = start};
      return 0;
    }
  }
  return V_PBUS_EBUSY; // table full
}

int v_pbus_open(const char *name) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc1(SYS_pbus_open, (uintptr_t)name);
#endif
  pbus_dev_t *dev = NULL;
  for (int i = 0; i < PBUS_MAX_DEVS && !dev; i++)
    if (pbus_devs[i].name && name_eq(pbus_devs[i].name, name))
      dev = &pbus_devs[i];
  if (!dev)
    return V_PBUS_EINVAL;

  pbus_handle_t *h = NULL;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  for (int i = 0; i < VAIOS_PBUS_MAX_OPEN && !h; i++)
    if (!pbus_handles[i].used)
      h = &pbus_handles[i];
  if (h)
    h->used = 1;
  EXIT_CRITICAL_FROM_ISR(s);
  if (!h)
    return V_PBUS_EBUSY; // no free handle

  h->dev = dev;
  h->job = (v_pbus_job_t){
      .start = handle_start, .done = handle_done, .arg = h};
  h->submitted = 0;
  h->orphan = 0;
  int fd = v_fd_alloc(&pbus_ops, h);
  if (fd < 0)
    h->used = 0;
  return fd;
}

int v_pbus_xfer_submit(int fd, const v_pbus_xfer_t *x, uint8_t prio) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc3(SYS_pbus_submit, (uint32_t)fd, (uintptr_t)x, prio);
#endif
  pbus_handle_t *h = (pbus_handle_t *)v_fd_obj(fd, &pbus_ops);
  if (!h || !x || x->tx_len > VAIOS_PBUS_XFER_MAX ||
      x->rx_len > VAIOS_PBUS_XFER_MAX || (x->tx_len && !x->tx))
    return V_PBUS_EINVAL;
  if (h->submitted || h->job.state != JOB_IDLE)
    return V_PBUS_EBUSY; // unfinished, or a timed-out one still on the bus

  if (x->tx_len)
    v_memcpy(h->tx, x->tx, x->tx_len);
  h->x = (v_pbus_xfer_t){.addr = x->addr, .flags = x->flags, .tx = h->tx,
                         .tx_len = x->tx_len, .rx = h->rx,
                         .rx_len = x->rx_len};
  // Fresh semaphore: drops a stale give from a transfer that finished after
  // its caller had already timed out.
  h->done = v_semaphore_create_binary_static(&h->done_buf);
  h->rc = 0;
  h->job.prio = prio;
  h->submitted = 1;
  if (v_pbus_submit(h->dev->bus, &h->job) != VA_PASS) {
    h->submitted = 0;
    return V_PBUS_EBUSY;
  }
  return 0;
}

// Block until the transfer completes or `ticks` pass. The outcome is read by
// v_pbus_xfer_finish, so the (possibly deferred) take result doesn't matter.
int v_pbus_xfer_wait(int fd, uint32_t ticks) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc2(SYS_pbus_wait, (uint32_t)fd, ticks);
#endif
  pbus_handle_t *h = (pbus_handle_t *)v_fd_obj(fd, &pbus_ops);
  if (!h || !h->submitted)
    return V_PBUS_EINVAL;
  return v_semaphore_take(h->done, ticks);
}

int v_pbus_xfer_finish(int fd, void *rx, uint32_t rx_len) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc3(SYS_pbus_finish, (uint32_t)fd, (uintptr_t)rx, rx_len);
#endif
  pbus_handle_t *h = (pbus_handle_t *)v_fd_obj(fd, &pbus_ops);
  if (!h || !h->submitted)
    return V_PBUS_EINVAL;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  uint8_t state = h->job.state;
  if (state == JOB_QUEUED)
    unqueue(h->dev->bus, &h->job);
  h->submitted = 0;
  EXIT_CRITICAL_FROM_ISR(s);
  if (state == JOB_QUEUED)
    return V_PBUS_ETIMEDOUT;
  if (state == JOB_ACTIVE)
    return V_PBUS_EBUSY; // still on the bus; its result is dropped
  uint32_t n = rx_len < h->x.rx_len ? rx_len : h->x.rx_len;
  if (n && rx)
    v_memcpy(rx, h->rx, n);
  return h->rc;
}

int v_pbus_xfer(int fd, const v_pbus_xfer_t *x, uint8_t prio, uint32_t ticks) {
  int rc = v_pbus_xfer_submit(fd, x, prio);
  if (rc < 0)
    return rc;
  v_pbus_xfer_wait(fd, ticks);
  return v_pbus_xfer_finish(fd, x->rx, x->rx_len);
}
#endif // VAIOS_DEVFS
