#include "ipc.h"
#include "atomic.h"
#include "memory.h"
#include "perf_hooks.h"
#include "port.h" // ENTER_CRITICAL / EXIT_CRITICAL[_FROM_ISR]
#include "syscall.h" // SVC trap wrappers (VAIOS_SYSCALL_SVC)
#include "vfile.h" // fd table (VAIOS_IPC_FD)
#include "task.h"
#include "utils.h"

// Mutex acquire/release cores (defined below): shared by the pointer API and,
// under VAIOS_IPC_FD, the fd-typed API, which is defined earlier in the file.
static int mutex_lock_common(rmutex_t *rm, uint32_t ticks_to_wait);
static int mutex_unlock_common(rmutex_t *rm);
#if VAIOS_IPC_FD
static void mwait_wake_observers(sema_t *s); // wake v_wait() watchers of s
#endif

//-----------------------------------------------------------------------------
// Wait Queue Helpers (priority-ordered, highest priority at head)
//-----------------------------------------------------------------------------
// Insert sorted by priority, most urgent at the head, so wait_q_dequeue
// (which pops the head) always hands the slot to the highest-priority
// waiter. Ties keep FIFO order among equal priorities, so a slow
// low-priority waiter can no longer delay a high-priority control task
// blocked on the same semaphore. O(n) in waiters on this one semaphore,
// which is bounded and typically 1-3.
// The wait queue threads through task->wait_next — never task->next — because
// a task blocked on a timed take also sits on blocked_list (via next/prev) at
// the same time.
static void wait_q_enqueue(sema_t *sem, TCB *task) {
  task->wait_next = NULL;

  // Empty queue, or task outranks the current head -> new head.
  if (!sem->wait_q || sem->wait_q->priority < task->priority) {
    task->wait_next = sem->wait_q;
    sem->wait_q = task;
    if (!task->wait_next)
      sem->tail = task;
    return;
  }

  // Walk past every waiter with priority >= task's (keeps ties FIFO).
  TCB *p = sem->wait_q;
  while (p->wait_next && p->wait_next->priority >= task->priority)
    p = p->wait_next;

  task->wait_next = p->wait_next;
  p->wait_next = task;
  if (!task->wait_next)
    sem->tail = task;
}

static TCB *wait_q_dequeue(sema_t *sem) {
  TCB *task = sem->wait_q;
  if (!task)
    return NULL;

  sem->wait_q = task->wait_next;
  if (!sem->wait_q)
    sem->tail = NULL;

  task->wait_next = NULL;
  return task;
}

// Unlink an arbitrary task from the queue (used when a blocked task is
// force-terminated, so a later give/handoff never touches a dead TCB).
static void wait_q_remove(sema_t *sem, TCB *task) {
  TCB **pp = &sem->wait_q;
  TCB *prev = NULL;
  while (*pp && *pp != task) {
    prev = *pp;
    pp = &(*pp)->wait_next;
  }
  if (!*pp)
    return; // not queued here
  *pp = task->wait_next;
  if (sem->tail == task)
    sem->tail = prev;
  task->wait_next = NULL;
}

//-----------------------------------------------------------------------------
// Semaphore / Mutex Creation
//-----------------------------------------------------------------------------
static sema_t *sema_init(sema_t *sem, uint32_t initial_count, uint32_t limit) {
  sem->wait_q = NULL;
  sem->tail = NULL;
  atomic_set(&sem->count, initial_count);
  atomic_set(&sem->limit, limit);
#if VAIOS_IPC_FD
  sem->observers = NULL;
#endif
  return sem;
}

SemaphoreHandle_t v_semaphore_create_binary(void) {
  sema_t *sem = (sema_t *)v_malloc(sizeof(sema_t));
  if (!sem)
    return NULL;
  return (SemaphoreHandle_t)sema_init(sem, 0, 1);
}

SemaphoreHandle_t
v_semaphore_create_binary_static(StaticSemaphore_t *pxBuffer) {
  if (!pxBuffer)
    return NULL;
  return (SemaphoreHandle_t)sema_init((sema_t *)pxBuffer, 0, 1);
}

SemaphoreHandle_t v_semaphore_create_counting(uint32_t max_count,
                                              uint32_t initial_count) {
  if (initial_count > max_count)
    return NULL;
  sema_t *sem = (sema_t *)v_malloc(sizeof(sema_t));
  if (!sem)
    return NULL;
  return (SemaphoreHandle_t)sema_init(sem, initial_count, max_count);
}

SemaphoreHandle_t
v_semaphore_create_counting_static(uint32_t max_count, uint32_t initial_count,
                                   StaticSemaphore_t *pxBuffer) {
  if (!pxBuffer || initial_count > max_count)
    return NULL;
  return (SemaphoreHandle_t)sema_init((sema_t *)pxBuffer, initial_count,
                                      max_count);
}

MutexHandle_t v_mutex_create(void) {
  rmutex_t *mtx = (rmutex_t *)v_malloc(sizeof(rmutex_t));
  if (!mtx)
    return NULL;
  sema_init(&mtx->base, 1, 1);
  mtx->owner = NULL;
  mtx->recursion_count = 0;
  mtx->next_held = NULL;
  return (MutexHandle_t)mtx;
}

MutexHandle_t v_mutex_create_static(StaticSemaphore_t *pxBuffer) {
  if (!pxBuffer)
    return NULL;
  rmutex_t *mtx = (rmutex_t *)pxBuffer;
  sema_init(&mtx->base, 1, 1);
  mtx->owner = NULL;
  mtx->recursion_count = 0;
  mtx->next_held = NULL;
  return (MutexHandle_t)mtx;
}

MutexHandle_t v_mutex_create_recursive(void) {
  rmutex_t *mtx = (rmutex_t *)v_malloc(sizeof(rmutex_t));
  if (!mtx)
    return NULL;
  sema_init(&mtx->base, 1, 1);
  mtx->owner = NULL;
  mtx->recursion_count = 0;
  mtx->next_held = NULL;
  return (MutexHandle_t)mtx;
}

MutexHandle_t v_mutex_create_recursive_static(StaticSemaphore_t *pxBuffer) {
  if (!pxBuffer)
    return NULL;
  rmutex_t *mtx = (rmutex_t *)pxBuffer;
  sema_init(&mtx->base, 1, 1);
  mtx->owner = NULL;
  mtx->recursion_count = 0;
  mtx->next_held = NULL;
  return (MutexHandle_t)mtx;
}

//-----------------------------------------------------------------------------
// Semaphore / Mutex Operations
//-----------------------------------------------------------------------------
// Absolute wake deadline for a FINITE timeout. delay_ticks == 0 is the
// "no deadline" (V_WAIT_FOREVER) sentinel, so a finite deadline that lands
// exactly on 0 — the 32-bit tick counter summing past UINT32_MAX — is nudged to
// 1. Otherwise a finite wait silently becomes infinite. (STAGE5_REVIEW #14.)
static inline uint32_t v_wake_deadline(uint32_t ticks_to_wait) {
  uint32_t d = v_get_ticks() + ticks_to_wait;
  return d == 0u ? 1u : d;
}

static int semaphore_take_common(sema_t *s, uint32_t ticks_to_wait) {
  TCB *current = get_current_task();
  PERF_IPC_TAKE_ATTEMPT();
  ENTER_CRITICAL();

  if (atomic_get(&s->count) > 0) {
    atomic_dec(&s->count);
    EXIT_CRITICAL();
    return VA_PASS;
  }

  if (ticks_to_wait == 0) {
    EXIT_CRITICAL();
    return VA_FAIL; // would block
  }

  // Block task and enqueue in semaphore wait queue. V_WAIT_FOREVER parks the
  // waiter with delay_ticks == 0, which the SysTick timeout scan treats as "no
  // deadline" (it only ejects waiters with a nonzero, expired delay_ticks) — so
  // the task waits until an explicit give/handoff, never a timeout.
  current->status = TASK_BLOCKED;
  current->delay_ticks =
      (ticks_to_wait == V_WAIT_FOREVER) ? 0u : v_wake_deadline(ticks_to_wait);
  current->wait_sem = s; // track which semaphore we are waiting on
  wait_q_enqueue(s, current);
  add_to_blocked_list(
      current); // so wake_up_delayed_tasks can find us on timeout

  PERF_IPC_TAKE_BLOCKED();
  // Leave the critical section BEFORE pending the switch. Commit 5b3df99 had
  // reordered these (task_yield while still BASEPRI-masked, EXIT_CRITICAL after)
  // to make block-and-yield atomic. On real STM32F401 that opened a give<->take
  // race: a DMA-completion v_semaphore_give_from_isr landing between the
  // BLOCKED-mark and the taken switch left the waiting task's wake lost, so it
  // always fell through to its timeout — the IMU DMA reader got stuck in 20 Hz
  // recovery (task_count frozen) and the estimator diverged to a bank-angle
  // failsafe. Confirmed on-target; the NavHAL maskable-IRQ-priority fix is the
  // real orphan-bug cure, so dropping the reorder is safe.
  EXIT_CRITICAL();
  v_port_trigger_pendsv(); // kernel-internal: pend directly (never via SVC)

#if VAIOS_SYSCALL_SVC
  if (!v_in_thread_mode()) {
    // Reached via a syscall (handler mode): the switch is deferred to SVCall
    // return, so we cannot resume-check inline (wait_sem is still set). Return a
    // placeholder; the waker (give / timeout) stashes the real result and PendSV
    // delivers it into our stacked r0 when we are next scheduled in.
    return V_SYSCALL_BLOCKED;
  }
#endif
  // Thread-mode (direct call) or SVC off: the switch already happened, so resume
  // here. semaphore_give cleared wait_sem → VA_PASS; a timeout left it set →
  // VA_FAIL.
  if (current->wait_sem != NULL) {
    current->wait_sem = NULL;
    PERF_IPC_TIMEOUT();
    return VA_FAIL;
  }
  return VA_PASS;
}

int v_semaphore_take(SemaphoreHandle_t sem, uint32_t ticks_to_wait) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode()) // task-facing: trap into the kernel
    return v_svc2(SYS_sem_take, (uint32_t)(uintptr_t)sem, ticks_to_wait);
#endif
  if (!sem)
    return VA_FAIL;
  return semaphore_take_common((sema_t *)sem, ticks_to_wait);
}

// Highest priority among the tasks queued on a mutex (0 if none). The wait
// queue is priority-ordered, so the head is the most urgent waiter.
static uint32_t mutex_top_waiter_prio(rmutex_t *rm) {
  return rm->base.wait_q ? rm->base.wait_q->priority : 0;
}

// Core mutex acquire (no trap guard): shared by the pointer API (v_mutex_lock)
// and the fd API (v_mtx_lock). Runs privileged, in handler mode on the syscall
// path. Returns VA_PASS / VA_FAIL, or V_SYSCALL_BLOCKED when it deferred.
static int mutex_lock_common(rmutex_t *rm, uint32_t ticks_to_wait) {
  TCB *current = get_current_task();

  ENTER_CRITICAL();
  // Transitive priority inheritance: walk the ownership chain and boost
  // every owner outranked by `current`. Each hop follows the boosted owner's
  // own wait_mutex (the mutex it is itself blocked acquiring), so a chain
  // low -> mid -> high resolves end to end. Capped at MAX_PI_DEPTH — a
  // deeper chain is a dependency cycle and panics.
  {
    rmutex_t *m = rm;
    int depth = 0;
    while (m && m->owner && m->owner->priority < current->priority) {
      if (++depth > MAX_PI_DEPTH)
        v_panic(__FILE__, __LINE__,
                "priority-inheritance chain exceeded MAX_PI_DEPTH (cycle?)");
      TCB *o = m->owner;
      task_change_priority(o, current->priority);
      // Propagate only while the boosted owner is itself blocked on a mutex.
      if (o->status != TASK_BLOCKED || !o->wait_mutex)
        break;
      m = (rmutex_t *)o->wait_mutex;
    }
  }
  current->wait_mutex = rm; // visible to other chain walks while we block
  EXIT_CRITICAL();

  int res = semaphore_take_common(&rm->base, ticks_to_wait);

#if VAIOS_SYSCALL_SVC
  if (res == V_SYSCALL_BLOCKED) {
    // Blocked via syscall (handler mode). On grant, v_mutex_unlock's direct
    // handoff already set ownership and cleared wait_mutex; on timeout the
    // ejector clears both. The VA_PASS/VA_FAIL result reaches our stacked r0 on
    // resume — nothing more to do here.
    return res;
  }
#endif

  ENTER_CRITICAL();
  current->wait_mutex = NULL;
  // Set ownership only for an UNCONTENDED acquire; a contended-then-granted lock
  // already had ownership handed to it by v_mutex_unlock's direct handoff.
  if (res == VA_PASS && rm->owner != current) {
    rm->owner = current;
    rm->recursion_count = 1;
    // Track on the owner's held list for the unlock-time priority recompute.
    rm->next_held = (rmutex_t *)current->held_mutexes;
    current->held_mutexes = rm;
  }
  EXIT_CRITICAL();
  return res;
}

int v_mutex_lock(MutexHandle_t mtx, uint32_t ticks_to_wait) {
  if (!mtx)
    return VA_FAIL;
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode()) // task-facing: trap into the kernel
    return v_svc2(SYS_mutex_lock, (uint32_t)(uintptr_t)mtx, ticks_to_wait);
#endif
  return mutex_lock_common((rmutex_t *)mtx, ticks_to_wait);
}

static int semaphore_give_common(sema_t *s, int *pxHigherPriorityTaskWoken) {
  PERF_IPC_GIVE();
  // FromISR-safe critical section: this function is shared by v_semaphore_give
  // (task context) and v_semaphore_give_from_isr (ISR context). Saving/
  // restoring BASEPRI in a local — instead of bumping the global
  // critical_nesting — keeps the ISR path from corrupting a preempted task's
  // critical-section accounting. The atomic_* below are lock-free, so they take
  // no critical section of their own.
  uint32_t isr_state = ENTER_CRITICAL_FROM_ISR();

  // Unblock first waiting task if any — hand off the slot directly
  TCB *to_unblock = wait_q_dequeue(s);
  if (to_unblock) {
    // Clear wait_sem so take() knows it was given (not a timeout).
    // Also remove from blocked_list since we are no longer waiting.
    to_unblock->wait_sem = NULL;
    remove_from_blocked_list(to_unblock);
    to_unblock->status = TASK_READY;
    add_to_ready_list(to_unblock);
#if VAIOS_SYSCALL_SVC
    // Deliver VA_PASS to a syscall-blocked take() when it next runs.
    v_syscall_wake_result(to_unblock, VA_PASS);
#endif

    if (pxHigherPriorityTaskWoken &&
        to_unblock->priority > get_current_task()->priority)
      *pxHigherPriorityTaskWoken = 1;

    EXIT_CRITICAL_FROM_ISR(isr_state);
    return VA_PASS;
  }

  if (atomic_get(&s->count) >= atomic_get(&s->limit)) {
    EXIT_CRITICAL_FROM_ISR(isr_state);
    return VA_FAIL; // full
  }

  atomic_inc(&s->count);
#if VAIOS_IPC_FD
  mwait_wake_observers(s); // a take would now succeed: wake v_wait() watchers
#endif
  EXIT_CRITICAL_FROM_ISR(isr_state);
  return VA_PASS;
}

int v_semaphore_give(SemaphoreHandle_t sem) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode()) // task-facing: trap into the kernel
    return v_svc1(SYS_sem_give, (uint32_t)(uintptr_t)sem);
#endif
  if (!sem)
    return VA_FAIL;
  return semaphore_give_common((sema_t *)sem, NULL);
}

#if VAIOS_IPC_FD
// --- fd-typed named semaphores (Stage 3) ------------------------------------
// A named sem lives in this kernel table; tasks reference it by a per-task fd
// (allocated in the fd table with ipc_sem_ops). Sharing is by name: each task
// v_sem_open()s the same name and gets its own fd to the same object.
#define MAX_NAMED_SEMS 8
typedef struct {
  char name[16];
  sema_t sem;
  int refcount;
  uint8_t used;
} named_sem_t;
static named_sem_t named_sems[MAX_NAMED_SEMS];

static int sname_eq(const char *a, const char *b) {
  int i = 0;
  while (a[i] && a[i] == b[i])
    i++;
  return a[i] == b[i];
}
static named_sem_t *named_sem_find(const char *name) {
  for (int i = 0; i < MAX_NAMED_SEMS; i++)
    if (named_sems[i].used && sname_eq(named_sems[i].name, name))
      return &named_sems[i];
  return NULL;
}

// fd close op: drop a reference; reclaim the named sem at refcount 0.
static int ipc_sem_close(void *priv) {
  named_sem_t *ns = (named_sem_t *)priv;
  uint32_t st = ENTER_CRITICAL_FROM_ISR();
  if (ns->refcount > 0 && --ns->refcount == 0)
    ns->used = 0;
  EXIT_CRITICAL_FROM_ISR(st);
  return 0;
}
static const v_file_ops ipc_sem_ops = {
    .read = NULL, .write = NULL, .close = ipc_sem_close};

// A named-object name must fit the fixed 16-byte slot (15 chars + NUL). A longer
// name would be silently truncated on store and then never match on re-open —
// each open()ing a second, distinct object that can never rendezvous. Reject it.
static int ipc_name_ok(const char *name) {
  int n = 0;
  while (n < 16 && name[n])
    n++;
  return n < 16; // false once length reaches 16
}

int v_sem_open(const char *name, int flags) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc2(SYS_sem_open, (uint32_t)(uintptr_t)name, (uint32_t)flags);
#endif
  if (!ipc_name_ok(name))
    return -1;
  ENTER_CRITICAL();
  named_sem_t *ns = named_sem_find(name);
  if (!ns) {
    if (!(flags & V_IPC_CREATE)) {
      EXIT_CRITICAL();
      return -1;
    }
    for (int i = 0; i < MAX_NAMED_SEMS; i++)
      if (!named_sems[i].used) {
        ns = &named_sems[i];
        break;
      }
    if (!ns) {
      EXIT_CRITICAL();
      return -1;
    }
    int j = 0;
    while (j < 15 && name[j]) {
      ns->name[j] = name[j];
      j++;
    }
    ns->name[j] = '\0';
    sema_init(&ns->sem, 0, 1); // binary
    ns->refcount = 0;
    ns->used = 1;
  }
  ns->refcount++;
  EXIT_CRITICAL();
  int fd = v_fd_alloc(&ipc_sem_ops, ns);
  if (fd < 0)
    ipc_sem_close(ns); // undo the ref we took
  return fd;
}

int v_sem_take(int fd, uint32_t ticks_to_wait) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc2(SYS_sem_take_fd, (uint32_t)fd, ticks_to_wait);
#endif
  named_sem_t *ns = (named_sem_t *)v_fd_obj(fd, &ipc_sem_ops);
  if (!ns)
    return VA_FAIL;
  return semaphore_take_common(&ns->sem, ticks_to_wait);
}

int v_sem_give(int fd) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc1(SYS_sem_give_fd, (uint32_t)fd);
#endif
  named_sem_t *ns = (named_sem_t *)v_fd_obj(fd, &ipc_sem_ops);
  if (!ns)
    return VA_FAIL;
  return semaphore_give_common(&ns->sem, NULL);
}

// --- fd-typed named mutexes -------------------------------------------------
// Same model as named semaphores, but the object is a priority-inheriting
// mutex. Lock/unlock resolve fd -> rmutex_t and reuse the pointer API's cores.
#define MAX_NAMED_MTXS 8
typedef struct {
  char name[16];
  rmutex_t mtx;
  int refcount;
  uint8_t used;
} named_mtx_t;
static named_mtx_t named_mtxs[MAX_NAMED_MTXS];

static named_mtx_t *named_mtx_find(const char *name) {
  for (int i = 0; i < MAX_NAMED_MTXS; i++)
    if (named_mtxs[i].used && sname_eq(named_mtxs[i].name, name))
      return &named_mtxs[i];
  return NULL;
}

static int ipc_mtx_close(void *priv) {
  named_mtx_t *nm = (named_mtx_t *)priv;
  uint32_t st = ENTER_CRITICAL_FROM_ISR();
  if (nm->refcount > 0 && --nm->refcount == 0)
    nm->used = 0;
  EXIT_CRITICAL_FROM_ISR(st);
  return 0;
}
static const v_file_ops ipc_mtx_ops = {
    .read = NULL, .write = NULL, .close = ipc_mtx_close};

int v_mtx_open(const char *name, int flags) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc2(SYS_mtx_open, (uint32_t)(uintptr_t)name, (uint32_t)flags);
#endif
  if (!ipc_name_ok(name))
    return -1;
  ENTER_CRITICAL();
  named_mtx_t *nm = named_mtx_find(name);
  if (!nm) {
    if (!(flags & V_IPC_CREATE)) {
      EXIT_CRITICAL();
      return -1;
    }
    for (int i = 0; i < MAX_NAMED_MTXS; i++)
      if (!named_mtxs[i].used) {
        nm = &named_mtxs[i];
        break;
      }
    if (!nm) {
      EXIT_CRITICAL();
      return -1;
    }
    int j = 0;
    while (j < 15 && name[j]) {
      nm->name[j] = name[j];
      j++;
    }
    nm->name[j] = '\0';
    sema_init(&nm->mtx.base, 1, 1); // mutex: initially available
    nm->mtx.owner = NULL;
    nm->mtx.recursion_count = 0;
    nm->mtx.next_held = NULL;
    nm->refcount = 0;
    nm->used = 1;
  }
  nm->refcount++;
  EXIT_CRITICAL();
  int fd = v_fd_alloc(&ipc_mtx_ops, nm);
  if (fd < 0)
    ipc_mtx_close(nm); // undo the ref we took
  return fd;
}

int v_mtx_lock(int fd, uint32_t ticks_to_wait) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc2(SYS_mtx_lock_fd, (uint32_t)fd, ticks_to_wait);
#endif
  named_mtx_t *nm = (named_mtx_t *)v_fd_obj(fd, &ipc_mtx_ops);
  if (!nm)
    return VA_FAIL;
  // Non-recursive: a re-lock by the current owner would otherwise fall into the
  // blocking path (count is 0) and self-deadlock — it can never be woken.
  // Return an error instead. (The recursive API is v_mutex_lock_recursive.)
  if (nm->mtx.owner == get_current_task())
    return VA_FAIL;
  return mutex_lock_common(&nm->mtx, ticks_to_wait);
}

int v_mtx_unlock(int fd) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc1(SYS_mtx_unlock_fd, (uint32_t)fd);
#endif
  named_mtx_t *nm = (named_mtx_t *)v_fd_obj(fd, &ipc_mtx_ops);
  if (!nm)
    return VA_FAIL;
  return mutex_unlock_common(&nm->mtx);
}

// --- v_sem_poll / v_wait ----------------------------------------------------
// Non-consuming readiness probe: 1 if a v_sem_take(fd) would succeed right now
// (the named sem has a pending give), 0 if it would block, negative on bad fd.
int v_sem_poll(int fd) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc1(SYS_sem_poll, (uint32_t)fd);
#endif
  named_sem_t *ns = (named_sem_t *)v_fd_obj(fd, &ipc_sem_ops);
  if (!ns)
    return -1;
  return atomic_get(&ns->sem.count) > 0 ? 1 : 0;
}

// --- multi-fd wait (v_wait): O(1) blocking on a set of sem fds --------------
// A v_wait()ing task links a wnode into each watched sem's observer list and
// blocks on blocked_list with a deadline. When any watched sem's count goes
// positive, the give path calls mwait_wake_observers to wake every registered
// task; the woken task unlinks its nodes and reports which fd is ready. Blocking
// and timeout reuse the sem_take machinery: the SysTick scan ejects a timed-out
// waiter (wake_up_delayed_tasks_isr, which clears in_multiwait), and the wake
// result is delivered through the same deferred-result path. No polling.
//
// Delivered result convention: SYS_wait returns a ready index >= 0 when a fd is
// already ready (nothing armed), else it parks; the waker/timeout delivers a
// NEGATIVE value, on which v_wait runs SYS_wait_disarm to unlink and report.
#define V_MWAIT_WOKEN (-2) // "blocked then woken" — go disarm and re-check

// Wake every task observing s. Called from give under the sem's critical
// section, right after count goes positive. Leaves the woken tasks' nodes
// linked (they unlink themselves in mwait_disarm); in_multiwait == 0 makes any
// further give on another watched sem skip an already-woken task.
static void mwait_wake_observers(sema_t *s) {
  for (v_wnode *n = s->observers; n; n = n->next) {
    TCB *t = n->owner;
    if (!t->in_multiwait)
      continue;
    t->in_multiwait = 0;
    t->delay_ticks = 0; // cancel the timeout deadline
    remove_from_blocked_list(t);
    t->status = TASK_READY;
    add_to_ready_list(t);
#if VAIOS_SYSCALL_SVC
    v_syscall_wake_result(t, V_MWAIT_WOKEN);
#endif
  }
}

// Unlink all of t's armed nodes and return the index of the first watched sem
// that is ready now (count > 0), or -1 if none (timeout / already consumed).
static int mwait_disarm(TCB *t) {
  int ready = -1;
  ENTER_CRITICAL();
  for (int i = 0; i < t->narm; i++) {
    v_wnode *node = &t->wnodes[i];
    sema_t *s = (sema_t *)node->sem;
    if (s) {
      v_wnode **pp = &s->observers; // unlink node from s->observers
      while (*pp && *pp != node)
        pp = &(*pp)->next;
      if (*pp)
        *pp = node->next;
      if (ready < 0 && atomic_get(&s->count) > 0)
        ready = node->idx;
    }
    node->sem = NULL;
    node->next = NULL;
  }
  t->narm = 0;
  EXIT_CRITICAL();
  return ready;
}

// SYS_wait body (handler mode on the syscall path). Returns a ready fds[] index
// immediately if one is ready (nothing armed), -1 for a non-blocking probe with
// nothing ready, or V_SYSCALL_BLOCKED after arming observers and parking.
int32_t v_wait_block_impl(const int *fds, int nfds, uint32_t ticks) {
  // Security bound: the arm loop writes one cur->wnodes[] entry per valid fd, so
  // nfds MUST be clamped to the array size here — not only in the v_wait()
  // wrapper, which an unprivileged task bypasses by issuing the SVC directly.
  if (nfds <= 0 || nfds > VAIOS_MAX_FDS)
    return -1;
  TCB *cur = get_current_task();
  ENTER_CRITICAL();
  // Readiness scan first: a ready fd needs neither blocking nor registration.
  for (int i = 0; i < nfds; i++) {
    named_sem_t *ns = (named_sem_t *)v_fd_obj(fds[i], &ipc_sem_ops);
    if (ns && atomic_get(&ns->sem.count) > 0) {
      EXIT_CRITICAL();
      return i;
    }
  }
  if (ticks == 0) {
    EXIT_CRITICAL();
    return -1; // non-blocking probe, nothing ready
  }
  // Arm: link a node into each valid sem's observer list.
  int k = 0;
  for (int i = 0; i < nfds; i++) {
    named_sem_t *ns = (named_sem_t *)v_fd_obj(fds[i], &ipc_sem_ops);
    if (!ns)
      continue; // skip bad fds
    v_wnode *node = &cur->wnodes[k++];
    node->sem = &ns->sem;
    node->owner = cur;
    node->idx = i;
    node->next = ns->sem.observers;
    ns->sem.observers = node;
  }
  cur->narm = (uint8_t)k;
  if (k == 0) { // no valid fds
    EXIT_CRITICAL();
    return -1;
  }
  cur->in_multiwait = 1;
  cur->status = TASK_BLOCKED;
  cur->delay_ticks = (ticks == V_WAIT_FOREVER) ? 0u : v_wake_deadline(ticks);
  add_to_blocked_list(cur);
  EXIT_CRITICAL();
  v_port_trigger_pendsv();

#if VAIOS_SYSCALL_SVC
  if (!v_in_thread_mode())
    return V_SYSCALL_BLOCKED; // deferred: disarm + result handled after resume
#endif
  return V_MWAIT_WOKEN; // SVC-off fallback: switch already happened
}

// SYS_wait_disarm body: unlink this task's nodes, return the ready index.
int32_t v_wait_disarm_impl(void) {
  return mwait_disarm(get_current_task());
}

// Block until any of nfds sem fds is ready (a take would not block) or the
// timeout elapses. Returns the fds[] index of a ready descriptor, or -1 on
// timeout / bad args. Non-consuming: the caller then v_sem_take()s that fd.
// ticks == V_WAIT_FOREVER waits indefinitely; ticks == 0 is a single probe.
int v_wait(const int *fds, int nfds, uint32_t ticks) {
  if (!fds || nfds <= 0 || nfds > VAIOS_MAX_FDS)
    return -1;
  int r = v_svc3(SYS_wait, (uint32_t)(uintptr_t)fds, (uint32_t)nfds, ticks);
  if (r >= 0)
    return r;                   // a fd was already ready (nothing was armed)
  return v_svc0(SYS_wait_disarm); // blocked-then-woken / timeout: unlink + report
}
#endif // VAIOS_IPC_FD

int vaios_isr_priority_is_safe(uint32_t vectactive, uint32_t irq_prio) {
  if (vectactive < 16u)
    return 1; // thread mode or a system handler — no NVIC priority to violate
  return irq_prio >= (uint32_t)MAX_SYSCALL_INTERRUPT_PRIORITY;
}

int v_semaphore_give_from_isr(SemaphoreHandle_t sem,
                              int *pxHigherPriorityTaskWoken) {
  if (!sem)
    return VA_FAIL;
#if VAIOS_FROMISR_PRIO_CHECK
  // Enforce the FromISR priority contract the scheduler's correctness relies on:
  // an IRQ more urgent than MAX_SYSCALL_INTERRUPT_PRIORITY is not masked by the
  // kernel's critical sections, so calling this from there can tear the
  // scheduler lists. Fail loudly and locatably instead of corrupting silently.
  {
    uint32_t vectactive = 0;
    uint32_t prio = v_port_hw_active_irq_priority(&vectactive);
    if (!vaios_isr_priority_is_safe(vectactive, prio))
      v_panic(__FILE__, __LINE__,
              "v_semaphore_give_from_isr called from an IRQ above the syscall "
              "priority; raise its NVIC priority to the maskable band");
  }
#endif
  return semaphore_give_common((sema_t *)sem, pxHigherPriorityTaskWoken);
}

// Core mutex release (no trap guard): shared by v_mutex_unlock and v_mtx_unlock.
static int mutex_unlock_common(rmutex_t *rm) {
  TCB *current = get_current_task();

  ENTER_CRITICAL();
  if (rm->owner != current) {
    EXIT_CRITICAL();
    return VA_FAIL; // Not the owner
  }

  // Unlink rm from the owner's held-mutex list.
  rmutex_t **pp = (rmutex_t **)&current->held_mutexes;
  while (*pp && *pp != rm)
    pp = &(*pp)->next_held;
  if (*pp)
    *pp = rm->next_held;
  rm->next_held = NULL;

  // Recompute the owner's priority: its base, raised to the highest-priority
  // waiter across the mutexes it STILL holds. Replaces the old unconditional
  // drop-to-base, which was wrong whenever the owner held another mutex that
  // still had a high-priority task waiting on it.
  uint32_t newp = current->base_priority;
  for (rmutex_t *m = (rmutex_t *)current->held_mutexes; m; m = m->next_held) {
    uint32_t w = mutex_top_waiter_prio(m);
    if (w > newp)
      newp = w;
  }
  if (newp != current->priority)
    task_change_priority(current, newp);

  // Direct handoff: transfer the mutex straight to the highest-priority waiter
  // so it resumes already owning it (its lock post-process never runs on the
  // syscall path — and on the thread path it sees rm->owner already == itself,
  // so it skips). Clear the waiter's wait_sem/wait_mutex here (it is no longer
  // blocked), keeping transitive-PI visibility correct until the actual grant.
  TCB *w = wait_q_dequeue(&rm->base);
  if (w) {
    w->wait_sem = NULL;
    w->wait_mutex = NULL;
    remove_from_blocked_list(w);
    w->status = TASK_READY;
    add_to_ready_list(w);
    rm->owner = w;
    rm->recursion_count = 1;
    rm->next_held = (rmutex_t *)w->held_mutexes;
    w->held_mutexes = rm;
#if VAIOS_SYSCALL_SVC
    v_syscall_wake_result(w, VA_PASS); // deliver to the blocked lock()
#endif
    EXIT_CRITICAL();
    return VA_PASS;
  }

  // No waiter: release the mutex (free the binary base slot).
  rm->owner = NULL;
  rm->recursion_count = 0;
  atomic_inc(&rm->base.count);
  EXIT_CRITICAL();
  return VA_PASS;
}

// Release everything a terminating task holds or waits on, so no later
// give/unlock/wake dereferences its (soon-freed) TCB. Called from the task exit
// paths (kernel/task.c) with the scheduler critical section already held, and
// works whether or not `t` is the running task:
//   - hands off every mutex t owns to its highest-priority waiter (or frees it),
//   - unlinks t from the raw-semaphore wait queue it is parked on,
//   - disarms its multi-fd wait observers.
void v_ipc_task_teardown(TCB *t) {
  // 1. Hand off / free every mutex t owns (mirror of mutex_unlock_common's
  //    handoff, but for `t` rather than the current task — t's own priority is
  //    irrelevant, it is terminating).
  while (t->held_mutexes) {
    rmutex_t *rm = (rmutex_t *)t->held_mutexes;
    t->held_mutexes = rm->next_held;
    rm->next_held = NULL;
    TCB *w = wait_q_dequeue(&rm->base);
    if (w) {
      w->wait_sem = NULL;
      w->wait_mutex = NULL;
      remove_from_blocked_list(w);
      w->status = TASK_READY;
      add_to_ready_list(w);
      rm->owner = w;
      rm->recursion_count = 1;
      rm->next_held = (rmutex_t *)w->held_mutexes;
      w->held_mutexes = rm;
#if VAIOS_SYSCALL_SVC
      v_syscall_wake_result(w, VA_PASS);
#endif
    } else {
      rm->owner = NULL;
      rm->recursion_count = 0;
      atomic_inc(&rm->base.count);
    }
  }
  // 2. If parked on a raw semaphore/mutex wait queue, unlink so a later
  //    give/handoff never wakes a dead task.
  if (t->wait_sem) {
    wait_q_remove(t->wait_sem, t);
    t->wait_sem = NULL;
  }
  t->wait_mutex = NULL;
#if VAIOS_IPC_FD
  // 3. If in a multi-fd wait, unlink its observer nodes.
  if (t->in_multiwait)
    (void)mwait_disarm(t);
#endif
#if VAIOS_DEVFS
  // 4. Close every fd the task still holds so named-object refcounts drop and
  //    their table slots are reclaimed (done AFTER the mutex handoff above, so a
  //    still-held mutex's slot isn't freed while it is being released). This is
  //    the fix for the named-object leak on exit.
  v_fd_close_all(t);
#endif
}

int v_mutex_unlock(MutexHandle_t mtx) {
  if (!mtx)
    return VA_FAIL;
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode()) // task-facing: trap into the kernel
    return v_svc1(SYS_mutex_unlock, (uint32_t)(uintptr_t)mtx);
#endif
  return mutex_unlock_common((rmutex_t *)mtx);
}

int v_mutex_lock_recursive(MutexHandle_t mtx, uint32_t ticks_to_wait) {
  rmutex_t *rm = (rmutex_t *)mtx;
  TCB *current = get_current_task();

  ENTER_CRITICAL();
  if (rm->owner == current) {
    rm->recursion_count++;
    EXIT_CRITICAL();
    return VA_PASS;
  }
  EXIT_CRITICAL();

  // otherwise, lock normally
  if (v_mutex_lock((MutexHandle_t)rm, ticks_to_wait) == VA_PASS) {
    return VA_PASS;
  }
  return VA_FAIL;
}

int v_mutex_unlock_recursive(MutexHandle_t mtx) {
  rmutex_t *rm = (rmutex_t *)mtx;

  ENTER_CRITICAL();
  if (rm->owner != get_current_task()) {
    EXIT_CRITICAL();
    return VA_FAIL; // not owner
  }

  rm->recursion_count--;
  if (rm->recursion_count == 0) {
    // Balance this function's ENTER_CRITICAL before delegating — v_mutex_unlock
    // runs its own critical section. Without this the nesting count never
    // returns to 0, BASEPRI is never cleared, and the kernel freezes.
    EXIT_CRITICAL();
    return v_mutex_unlock((MutexHandle_t)rm);
  }
  EXIT_CRITICAL();
  return VA_PASS;
}
