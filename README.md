# VAiOS

A small preemptive real-time operating system for ARM Cortex-M4, targeted at
the hard-real-time workloads of a 1 kHz flight controller. Built on top of
[NavHAL](https://github.com/ragnar-vallhala/NavHAL) for the STM32F4 hardware
abstraction layer; reference board is the Nucleo-F401RE.

## Features

- **Preemptive priority scheduler** — 8 priority levels, O(1) ready-list
  management with `clz` priority pick, SysTick-driven delayed/timeout wakeup,
  round-robin within a priority.
- **Priority inheritance** — transitive chain walk on `v_mutex_lock`,
  recompute-from-held-mutexes on unlock; semaphore wait queues are
  priority-ordered.
- **BASEPRI critical sections** — kernel-internal IRQs are masked at the
  syscall ceiling, so high-priority hardware IRQs (motor PWM, IMU EXTI, ESC
  telemetry) stay unmaskable.
- **Segregated heap allocator** — O(1) `v_malloc` / `v_free` with backward
  coalescing, eight size-class free lists; free-list links live in each free
  block's payload so the 16-byte header is unchanged.
- **IPC** — binary and counting semaphores, mutexes (incl. recursive), static
  and dynamic allocation, SPSC and MPMC lock-free FIFO queues.
- **Buffered, DMA-backed logging** with compile-time level gating
  (`VAIOS_KERNEL_LOG_LEVEL`).
- **Optional modules** — `terminal`, `vfs` (FatFs over SDIO), `semihosting`,
  and the FIFO structures are opt-out CMake options; a flight build with
  them off lands well under 40 KB.
- **Verified on STM32F401RE.** Examples cover IPC, priority inheritance, the
  FIFO queues, heap, and more.

## Performance

Benchmarked against FreeRTOS and Zephyr at matched optimisation
(`-O2` / `-Os`). vaios is fastest-of-three on nearly every malloc/free metric
and competitive on IPC; `control_loop_1khz_jitter` is ~0 µs. Full results and
the per-change story are in [`docs/changelog/README.md`](docs/changelog/README.md);
the campaign plan is in
[`docs/benchmark/VAIOS_FLIGHT_IMPROVEMENT_PLAN.md`](docs/benchmark/VAIOS_FLIGHT_IMPROVEMENT_PLAN.md).

## Build and flash (STM32 hardware)

```bash
git clone --recurse-submodules https://github.com/ragnar-vallhala/vaios.git
cd vaios

mkdir build && cd build
cmake -DNAVHAL=ON -DEXAMPLES=ON -DVAIOS_EXAMPLE=FIFO_TEST ..
cmake --build .

arm-none-eabi-objcopy -O binary examples/main examples/main.bin
st-flash --connect-under-reset write examples/main.bin 0x8000000
```

UART output (USART2, 115200 8N1) appears on the Nucleo's ST-Link VCP at
`/dev/ttyACM0`:

```bash
stty -F /dev/ttyACM0 115200 raw -echo
cat /dev/ttyACM0
```

Other examples: `PRIORITY_INVERSION`, `IPC_TEST`, `HEAP_ALLOCATOR`,
`MULTI_TASK`, `TERMINAL`, `UART`, … (see `examples/CMakeLists.txt`).

## Build options

| Option | Default | Purpose |
| ------ | ------- | ------- |
| `NAVHAL` | OFF | Build against the NavHAL submodule (required for real hardware) |
| `EXAMPLES` | OFF | Build the example application selected by `VAIOS_EXAMPLE` |
| `VAIOS_EXAMPLE` | `""` | Selects the example (e.g. `FIFO_TEST`, `IPC_TEST`) |
| `VAIOS_FPU` | ON | Use the hardware FPU |
| `VAIOS_MODULE_TERMINAL` | ON | Include the interactive terminal |
| `VAIOS_MODULE_VFS` | ON | Include the VFS / FatFs layer |
| `VAIOS_MODULE_SEMIHOSTING` | ON | Include semihosting I/O |
| `VAIOS_MODULE_FIFO` | ON | Include SPSC/MPMC FIFO data structures |
| `VAIOS_USE_BASEPRI` | 1 | Use BASEPRI (not `cpsid i`) for critical sections |
| `VAIOS_KERNEL_LOG_LEVEL` | `LOG_WARN` | Compile-time gate for kernel hot-path logging |
| `MAX_PI_DEPTH` | 4 | Cap on the transitive PI chain-walk |

### NavHAL configuration

The NavHAL Kconfig (`.config`) is owned in this repo as `navhal.config` and
copied into `extern/NavHAL/.config` at configure time, so the submodule stays
pristine. Edit `navhal.config` directly or run NavHAL's interactive menuconfig:

```bash
cd extern/NavHAL && python3 tools/kconfig.py --menuconfig
```

then save the result back into the repo's `navhal.config`.

## QEMU

Renode emulates the STM32F4 model well enough to boot vaios — but its DMA
stream-enable bit doesn't self-clear on transfer-complete, so the DMA-backed
logging busy-waits forever there. On real hardware the bit clears and logging
works. The QEMU `netduinoplus2` machine does not emulate the F4 RCC at all and
the firmware hangs in clock init. Use real hardware for end-to-end testing.

The Docker image still bundles a no-HAL QEMU build for smoke-testing the
kernel without peripherals:

```bash
docker build -t vaios-arm-qemu .
docker run --rm vaios-arm-qemu
```

## Repository layout

```
include/             Public API headers
kernel/              Scheduler, IPC, memory, logging, terminal, VFS
portable/cortex-m4/  ARM Cortex-M4 port (port.c/h, PendSV, SVCall, BASEPRI)
extern/NavHAL/       HAL submodule (clocks, GPIO, UART, DMA, SDIO, DWT)
examples/            Standalone example applications
tests/               Host-native unit tests (gcc, x86)
docs/changelog/      One file per discrete bug fix / improvement
docs/benchmark/      Cross-RTOS benchmark plan, results, and improvement plan
tools/               Flash, run, and Renode helper scripts
navhal.config        Authoritative NavHAL Kconfig (copied into the submodule)
```

## Documentation

- **Changelog** — [`docs/changelog/`](docs/changelog/) — every significant fix
  or improvement, with root cause, fix, and lesson. The
  [index `README.md`](docs/changelog/README.md) lists everything and reports
  the cross-RTOS benchmark results.
- **Improvement plan** —
  [`docs/benchmark/VAIOS_FLIGHT_IMPROVEMENT_PLAN.md`](docs/benchmark/VAIOS_FLIGHT_IMPROVEMENT_PLAN.md)
  and
  [`docs/benchmark/implementation_plan/`](docs/benchmark/implementation_plan/)
  — the gap analysis and phased execution plan that drove the 2026-05
  flight-profile campaign.
- **Doxygen** — `cd build && make doc` (when Doxygen is installed).
