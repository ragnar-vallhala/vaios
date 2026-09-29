# Writing an unprivileged task

Under `VAIOS_MPU_USER_SEPARATION` every task the application creates runs
unprivileged. This is the reference for what such a task may do, what stays
privileged and why, and the one rule that catches everyone.

## The rule that catches everyone

**A task may touch only its own stack, its own heap block, and read-only flash.**

Not a global. Not a `static`. Not even for reading. A file-scope counter, a
`volatile` progress flag shared between two tasks, a `static` lookup table in
RAM — each of those is a MemManage fault the first time the task touches it.

```c
static volatile int samples;        /* .bss — a fault, not a variable */

void my_task(void *arg) {
    samples++;                      /* <-- MPU fault here */
}
```

Two consequences worth internalising:

- **Tasks share nothing but kernel objects.** If two tasks need to agree on
  something, it goes through a topic, a queue, a semaphore or a file — never a
  shared variable. The on-target example `examples/58_flight_user.c` is written
  this way throughout: three tasks, one sensor ISR, no shared memory at all.
- **A pointer you pass to a child is not readable by the child.** `v_task_spawn`
  takes an `arg`, and it is opaque on purpose: the child has its own MPU region,
  so a pointer into the parent's block faults there. Pass an integer or a handle.

`const` data in flash is fine — string literals, const tables, function
pointers. That is why `say("...")`-style helpers and `v_bus_open("imu.raw", …)`
work: the literal lives in flash, which the task may read and execute.

Diagnostics hide this rule in plain sight. `GET_CURRENT_TASK_ID()` dereferences
`current_task`, a kernel global, so a debug print using it faults. Use
`v_task_info()`.

## What a task can call

| Area | Calls |
|---|---|
| Scheduling | `task_yield`, `task_delay`, `v_delay`, `task_delay_until`, `task_exit` |
| Time | `v_get_ticks` |
| Itself | `v_task_info` (id, priority, stack size, name), `v_perf_self_stats` |
| Its own tasks | `v_task_spawn`, `v_task_kill` (its own children only) |
| Console / devices | `v_file_open`, `v_file_read`, `v_file_write`, `v_file_close`, `v_printf` |
| IPC | `v_sem_open`/`take`/`give`/`poll`, `v_mtx_open`/`lock`/`unlock`, `v_wait` |
| Bus IPC | `v_bus_open`, `v_bus_send`, `v_bus_recv`, `v_bus_recv_wait`, `v_bus_wait` |
| Queues | `v_queue_open`, `v_queue_send`, `v_queue_recv`, `v_queue_try_*`, `v_queue_wait` |
| Files | `v_vfs_open`, `v_vfs_read`, `v_vfs_write`, `v_vfs_close`, `v_vfs_seek`, `v_vfs_flush`, `v_vfs_info`, `v_vfs_makedir`, `v_vfs_remove`, `v_vfs_diropen`, `v_vfs_dirnext` — all worker-backed (see below) |
| Peripherals | `v_pbus_open`, `v_pbus_xfer` (and the submit/wait/finish split) |
| Heap | `malloc`/`free`/`calloc`/`realloc` — only with `VAIOS_TASK_HEAP` on |
| Pure helpers | `v_memcpy`, `v_memset`, `v_str*`, `print_fmt_buf`, all `spsc_*` |

Everything in that table is a syscall or pure code. Nothing in it needs the task
to hold a kernel pointer.

## What stays privileged, and why

**Because it hands out a pointer into shared kernel memory:**
`v_bus_reserve`/`commit`, `v_bus_peek`/`release`, `spsc_write_ptr`/`read_ptr`,
`v_malloc` and the global heap, `get_current_task`, `task_snapshot_list`,
`task_get_name`. Granting a task a pointer into the bus pool would grant it every
topic's messages. A user-facing zero-copy path means a pipe whose ring is mapped
as that one task's own region — not a syscall.

**Because the kernel would end up calling user code with privilege:**
`v_pbus_submit` and the cyclic-job API (a job carries `start`/`done` function
pointers), `terminal register_command`, the bus's callback notification mode. A
task that wants callback semantics blocks in a thread of its own.

**Because the handle cannot be validated:** the raw-handle IPC API
(`v_semaphore_take`/`give`, `v_mutex_lock`/`unlock`) is refused with `-EPERM` for
an unprivileged caller — `args[0]` is a kernel pointer. The fd-typed twins are
the path.

**Because it is boot-time or ISR-context:** `v_init`, `v_system_init`,
`scheduler_start`, `v_devfs_register`, `v_pbus_register`, `v_bus_init`,
`v_bus_topic_declare`, `v_queue_register`, `v_vfs_mount`, every `*_from_isr` and
`*_isr` entry point. A board declares its objects; tasks open them by name.

**Because measuring it through a trap would be a lie:** `v_perf_cycles`. A trap
costs on the order of a hundred cycles, so a user-space cycle read cannot measure
anything finer than itself. `v_perf_self_stats` (counters) and `v_get_ticks`
(coarse time) are the honest granularity.

## Patterns that work

**Declare privileged, open by name.** Everything a task uses is registered at
init and opened by name, which is also what makes the fd table the capability
boundary: no name, no access.

```c
/* init, privileged */
v_bus_init(&bus, pool_blocks, pool_desc, 32, 40);
v_bus_topic_declare(&bus, &imu, "imu.raw", &OVERWRITE);
v_queue_register("/q/telem", &telem_q);
v_vfs_mount("/mnt/");

/* task, unprivileged */
int s = v_bus_open("imu.raw", V_BUS_RD);
int q = v_queue_open("/q/telem", V_Q_WR);
int f = v_file_open("/mnt/log.csv", VFS_O_WRONLY | VFS_O_CREAT);
```

**Sleep, don't poll.** `v_bus_recv_wait(fd, &rx, ticks)` and
`v_queue_recv(fd, &item, ticks)` park the task until a publisher or producer
arrives. Both are composed of two syscalls — a try and a wait — deliberately: a
syscall body cannot both block and copy, so a transfer that waited inside the
kernel would report the wait's success having copied nothing.

**Hold a cadence with `task_delay_until`.** `v_delay` sleeps *for* a duration and
drifts by however long the loop body took; `task_delay_until` targets an absolute
deadline and does not.

```c
uint32_t next = v_get_ticks();
for (;;) {
    control_step();
    task_delay_until(&next, 5);   /* 5 ms cadence, no accumulated drift */
}
```

**Own what you spawn.** A child is always unprivileged, may not outrank its
parent, and only its parent may end it — not a sibling, not a grandparent. When a
task exits, the tasks it spawned exit too, at any depth: its owner is gone, so
nobody is left who may end them.

**Expect `-14`.** `V_EFAULT` (-14) is the kernel refusing a pointer that is not
yours: a buffer outside your block, or flash where the kernel would write. It is
a refusal, not a fault — the task keeps running, which is the difference between
a validated syscall and a crash.

## A syscall must never wait for a peripheral

This one is structural, and it is worth understanding before adding any syscall
that touches hardware.

`SVCall` runs at level **7** on this port — `VAIOS_MAX_SYSCALL_PRIO_LEVEL`, the
same level the `BASEPRI` ceiling is derived from. While a syscall body executes,
every interrupt at or below that ceiling is masked: SysTick (level 14), PendSV
(15), and every peripheral IRQ (NavHAL defaults enabled lines to 8). So a
syscall that waits for an interrupt-driven completion waits forever:

- the completion IRQ cannot be taken, so the flag it would clear never clears;
- `wfi` never wakes, because the interrupt that would wake it is masked;
- and any timeout measured in milliseconds never expires either, because the
  millisecond counter is driven by the masked SysTick.

Observed on hardware with an SD transfer: `ICSR` VECTACTIVE = 11 (inside
SVCall), `BASEPRI=0`, `sd_busy=1`, and the tick frozen at 18 — the whole system
stopped, including a higher-priority task that wanted nothing from the
filesystem.

### Why level 7, and not the 0 it used to be

Until this was reviewed, `SVCall` ran at priority 0, the highest in the machine.
Nothing chose that: `v_port_hw_sched_irq_init` set SysTick and PendSV explicitly
and never mentioned SVCall, so `SHPR2` kept the ARMv7-M reset value. The
original hardware capture above recorded `SHPR2=0` as an observation, not a
decision.

Leaving SVCall at 0 is conventional where `SVC` is a one-shot trampoline that
starts the first task and is never executed again — the handler is a few
instructions and its priority barely matters. Here `SVC` is the entire syscall
surface: the allocator, the fd tables, bus publishes. At 0, each of those ran
strictly more protected than any critical section in the kernel, and that is not
an invariant anything relies on — kernel state is held by `BASEPRI`, which masks
to level 7.

It also quietly broke a promise. `VAIOS_MAX_SYSCALL_PRIO_LEVEL` says IRQs more
urgent than it "are never masked by a kernel critical section and MUST NOT call
any vaios API". At SVCall 0, every syscall masked exactly those IRQs for its
whole duration. NavHAL makes the same argument in the other direction: its
`hal_interrupt_enable` refuses to leave a peripheral at 0 precisely because
priority 0 is not maskable by an RTOS `BASEPRI` section.

At the ceiling instead, the two agree. An IRQ in the zero-latency band (levels
0..6) preempts a syscall, as documented — safe because that band is already
forbidden from calling any vaios API. Everything at or below the ceiling stays
masked for the syscall's duration, which is exactly what `ENTER_CRITICAL`
guarantees anyway.

**This does not loosen the rule above.** A syscall still cannot wait for a
peripheral, because SysTick sits at 14 — below the ceiling, so still masked. The
rule follows from the ceiling, never from SVCall having been at 0.

Verified by reading the registers off both running boards — an F401 (Cortex-M4)
and a Nucleo-F767ZI (Cortex-M7): `SHPR2` is `0x70000000` (level 7) and `SHPR3`
is `0xe0f00000` (SysTick 14, PendSV 15) on each. The two share one port, so the
same `v_port_hw_sched_irq_init` sets both.

Nothing occupies levels 0–6 today — NavHAL defaults enabled IRQs to 8 — so this
was a latent contract violation rather than a live fault. That is also why it
was cheap to correct now, before a sensor ISR wants the latency it was promised.

**The pattern that works** is the one the peripheral bus and the bus/queue
blocking paths already use: split the operation so the *waiting* happens in
thread mode, under the scheduler, not in the handler.

```
SYS_pbus_submit   start the transfer, return immediately
SYS_pbus_wait     block the CALLER through the scheduler (deferred result)
SYS_pbus_finish   collect it
```

A syscall may block the calling task (the scheduler then runs someone else), and
it may do bounded CPU work. It may not sit in a loop waiting for hardware.

**This is why files have a worker.** A task's file call is `v_vfs_open` /
`read` / `write` / … , and each is submit + wait + finish underneath: the
arguments (and any write payload) are copied into a kernel slot, the caller
blocks on that slot through the scheduler, and a task you run yourself does the
transfer in thread mode with interrupts enabled.

```c
/* init, privileged */
v_vfs_mount("/mnt/");
/* PRIVILEGED: the worker touches the request slots, FatFs and the SDIO
   peripheral. task_create_privileged is reachable only from privileged code,
   because task_create* is not a syscall. */
task_create_privileged(vfsio_task, NULL, 2048, 1, "vfsio"); /* for(;;) v_vfs_worker_step(100); */

/* task, unprivileged */
int fd = v_vfs_open("/mnt/0:log.dat", VFS_O_WRONLY | VFS_O_CREAT, 500);
v_vfs_write(fd, rec, sizeof rec, 500);
v_vfs_close(fd, 500);
```

The worker's priority is yours to choose, because "when does logging happen
relative to control" is a flight decision. Measured on an F401: a task holding a
5 ms cadence saw **zero drift over 500 cycles** while another task wrote 3 KB to
the card and read it back — which is what settles the question the synchronous
version answered so badly. Until a worker has run once, file
calls fail with `-11` rather than blocking on nobody. Reads and writes are
chunked through a fixed bounce buffer, so kernel memory does not grow with what a
task asks for, and a task that dies with a file open has it CLOSED by the worker —
flushed, not leaked.

The devfs mount (`v_file_open("/mnt/…")`) still exists, but only for
**privileged** callers in thread mode, such as the bus snapshotter, where inline
I/O is safe.

## The reference application

`examples/58_flight_user.c`, run by `tools/renode_flight_user.sh`, is the worked
example and the release gate: a 1 kHz sensor ISR publishing to the bus, an
estimator blocking on it, a controller holding a 5 ms cadence, a logger draining
a telemetry queue, each task naming itself, reading its own counters, and
spawning and ending a worker of its own. Privileged code is init and the ISR;
everything else is unprivileged, and the runner asserts no MPU fault, no kernel
panic and no boundary complaints.
