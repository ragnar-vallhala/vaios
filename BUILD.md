# Building, Flashing, and Running VaiOS

VaiOS targets the ARM Cortex-M4 (reference board: STM32F401RE / Nucleo-F401RE)
but most of the kernel also builds and runs natively on a host PC for testing.
This document covers every build surface:

| Surface | What it is | Toolchain | Build dir | Section |
| ------- | ---------- | --------- | --------- | ------- |
| **Hardware** | Real STM32F401RE firmware | `arm-none-eabi-gcc` + NavHAL | `build/` | [Firmware](#1-firmware-build--flash-stm32-hardware) |
| **Host** | Kernel unit tests on x86 | `gcc` | `build_tests/` | [Host](#2-host-native-tests-no-board) |
| **Renode** | On-target emulation (SITL/PIL) | `arm-none-eabi-gcc` + NavHAL | `build_renode/`, `build_pil/` | [Renode](#3-renode-on-target-emulation) |
| **QEMU** | No-HAL kernel smoke test | `arm-none-eabi-gcc` | `build/` | [QEMU](#4-qemu-no-hal-smoke-test) |

There is also a single roll-up runner, [`tools/run_all_tests.sh`](#5-run-every-layer-at-once),
that executes whichever layers your machine has the tools for.

---

## 0. Prerequisites

Clone with the NavHAL submodule:

```bash
git clone --recurse-submodules https://github.com/ragnar-vallhala/vaios.git
cd vaios
# already cloned without --recurse-submodules?
git submodule update --init --recursive
```

Install the tools for the surfaces you care about:

| Tool | Needed for | Notes |
| ---- | ---------- | ----- |
| `cmake` (≥ 3.20), `make`, `gcc` | everything / host | |
| `arm-none-eabi-gcc` + newlib | hardware, Renode, QEMU | **Use 13.x (e.g. Ubuntu 24.04).** The 22.04 toolchain (10.3) miscompiles the kernel at `-O2` and the no-HAL examples HardFault under QEMU. |
| `arm-none-eabi-objcopy` | hardware (ELF → BIN) | ships with the toolchain |
| `st-flash`, `st-info` (stlink) | flashing / hardware tests | |
| `qemu-system-arm` | QEMU | |
| `renode` | Renode emulation | |
| `python3` | NavHAL Kconfig | |
| `docker` | containerized QEMU/dev shell | optional |

> **NavHAL build env:** any `-DNAVHAL=ON` build (hardware, Renode) needs the
> `srctree` environment variable exported so NavHAL's Kconfig resolves:
> ```bash
> export srctree="$PWD/extern/NavHAL"
> ```
> Export it before configuring, or the configure step fails in Kconfig.

---

## 1. Firmware build & flash (STM32 hardware)

The cross-compile build is the top-level `CMakeLists.txt`. The toolchain prefix
(`arm-none-eabi-`) is selected automatically. Pick one example with
`-DVAIOS_EXAMPLE=<NAME>`.

```bash
export srctree="$PWD/extern/NavHAL"

mkdir -p build && cd build
cmake -DNAVHAL=ON -DEXAMPLES=ON -DVAIOS_EXAMPLE=FIFO_TEST ..
cmake --build .
```

This produces `build/examples/main` (ELF). Flash it:

```bash
# manual
arm-none-eabi-objcopy -O binary examples/main examples/main.bin
st-flash --connect-under-reset write examples/main.bin 0x8000000

# or, from inside the build dir, use the generated target:
cmake --build . --target flash
```

Convenience wrapper that does configure + build + objcopy + flash in one shot:

```bash
tools/flash.sh FIFO_TEST     # builds in ./build and flashes
```

### Read UART output

USART2 (115200 8N1) appears on the Nucleo's ST-Link VCP at `/dev/ttyACM0`:

```bash
stty -F /dev/ttyACM0 115200 raw -echo
cat /dev/ttyACM0
```

### Available examples

Names passed to `-DVAIOS_EXAMPLE`:

```
BASIC_TASK         FIFO_TEST          PERF
BENCHMARK          FPU_USAGE          PERF_DEMO
BLOCKING           HEAP_ALLOCATOR     PRIORITY_INVERSION
DMA_DUMP           IPC_TEST           RACE_CONDITION
MEM_LEAK           MULTI_TASK         RACE_MUTEX_GUARD
SD_WRITE_TEST      STACK_OVERFLOW     STARVATION
SWITCHING          TERMINAL           UART
UNIT_TESTS         VFS_CONCURRENT     VFS_PERF
```

(authoritative list: `examples/CMakeLists.txt`). `UNIT_TESTS` is the on-target
test runner used by the Renode/hardware suites.

### Hardware regression suite

Builds, flashes, and UART-captures a curated set of examples on a connected
board, grepping for required PASS lines:

```bash
tools/run_hw_tests.sh                  # default port /dev/ttyACM0
PORT=/dev/ttyACM1 tools/run_hw_tests.sh
CAPTURE_SECS=20    tools/run_hw_tests.sh
```

Requires `arm-none-eabi-gcc`, `st-flash`, `st-info`, and a Nucleo on the port.
Exit code = number of failed examples.

---

## 2. Host-native tests (no board)

The kernel sources compile natively with `gcc` against stub headers in
`tests/stubs/` (they shadow the ARM port, `navhal.h`, and the VFS layer), so
**no ARM toolchain, board, or emulator is needed.** This is the CI path
(`.github/workflows/ci.yml`).

```bash
bash tools/run_tests.sh           # configure + build + run, with a summary table
bash tools/run_tests.sh --verbose
```

Builds into `build_tests/` and runs two binaries:

- `vaios_tests` — memory, task, scheduler, IPC, structure, VFS, vaios,
  terminal, perf
- `vaios_utils_tests` — the formatter, isolated to avoid symbol collisions

Exit code is non-zero on any failure.

### Manual host build (e.g. for an IDE / debugger)

```bash
cmake -S tests -B build_tests \
      -DCMAKE_C_COMPILER=gcc \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_SYSTEM_NAME=Linux \
      -DCMAKE_C_FLAGS="-DCORTEX_M4" \
      --fresh
cmake --build build_tests --parallel
./build_tests/vaios_tests
./build_tests/vaios_utils_tests
```

### Coverage

```bash
tools/coverage.sh                 # gcov line/branch report (needs gcc + gcov)
```

---

## 3. Renode (on-target emulation)

Renode boots the real NavHAL firmware on a generic STM32F4 model — the closest
thing to hardware without a board. Build a cross-compiled image first, then run
the single Renode script `tools/renode.resc`.

```bash
export srctree="$PWD/extern/NavHAL"
cmake -S . -B build_renode -DNAVHAL=ON -DEXAMPLES=ON -DVAIOS_EXAMPLE=MULTI_TASK
cmake --build build_renode

# from the repo root:
renode --console --disable-xwt -e "include @tools/renode.resc"
```

`tools/renode.resc` is parameterized by two variables (set them with `-e`
**before** the `include`):

| Variable | Default | Meaning |
| -------- | ------- | ------- |
| `$bin` | `@build_renode/examples/main` | ELF to load |
| `$run` | `"3.0"` | seconds of emulation for the main run |

It always: streams USART2 to the console, enables the **DWT cycle counter** at
84 MHz (so `v_perf_cycles()` / DWT timing work instead of faulting), quiets
Renode's incomplete-DMA register warnings, and prints a liveness probe
(`PC`, `systick_count`, `scheduler_running`, `current_task`) resolved from the
ELF symbol table at boot and after the run.

```bash
# On-target unit tests (the UNIT_TESTS image), run for 4 s:
cmake -S . -B build_pil -DNAVHAL=ON -DEXAMPLES=ON -DVAIOS_EXAMPLE=UNIT_TESTS
cmake --build build_pil
renode --console --disable-xwt \
  -e '$bin=@build_pil/examples/main' -e '$run="4.0"' \
  -e "include @tools/renode.resc"
# look for: ON-TARGET RESULT: ALL PASS
```

> **Note:** the DWT cycle counter only increments once the firmware enables it
> (`DEMCR.TRCENA` + `DWT_CTRL.CYCCNTENA`). The `UNIT_TESTS` image reads the
> counter but never enables it, so it reads 0 there; a perf example
> (`PERF_DEMO`, `BENCHMARK`) shows a live, advancing count.

---

## 4. QEMU (no-HAL smoke test)

QEMU runs a **no-HAL** build (`-DNAVHAL=OFF`) — the kernel without peripherals.
It is a smoke test only:

- The `netduinoplus2` machine **does not emulate the F4 RCC**, so a NavHAL
  build hangs in clock init — use no-HAL.
- A bare `-DNAVHAL=OFF -DEXAMPLES=ON` emits the libs but **no `examples/main`**,
  so QEMU has nothing to load. You must name an example.

The helper always passes one:

```bash
tools/qemu_no_hal.sh                       # FIFO_TEST by default
VAIOS_EXAMPLE=PERF_DEMO tools/qemu_no_hal.sh
```

> **FPU examples HardFault under QEMU.** The no-HAL build is hard-float
> (`VAIOS_FPU=ON`), but QEMU's `cortex-m4` model has no usable FPU here, so the
> first floating-point instruction takes a **NOCP UsageFault** (`CFSR` bit 19)
> that escalates to a HardFault / kernel panic. Integer examples (`FIFO_TEST`,
> `IPC_TEST`, …) are unaffected. To smoke-test an FP example (`PERF_DEMO`,
> `FPU_USAGE`, `BENCHMARK`) under QEMU, rebuild it soft-float:
> ```bash
> cmake -S . -B build -DNAVHAL=OFF -DEXAMPLES=ON \
>       -DVAIOS_EXAMPLE=PERF_DEMO -DVAIOS_FPU=OFF
> cmake --build build
> qemu-system-arm -M netduinoplus2 -cpu cortex-m4 \
>   -kernel build/examples/main -nographic -semihosting
> ```
> For real FPU behavior, run these in Renode or on hardware instead.

Under the hood it builds into `build/` and runs:

```bash
qemu-system-arm -M netduinoplus2 -cpu cortex-m4 \
  -kernel examples/main -nographic -d unimp,guest_errors -semihosting
```

Debug variant (waits for GDB on `:1234` via `-s -S`):

```bash
tools/qemu_no_hal_debug.sh
# then in another shell:
gdb-multiarch build/examples/main -ex 'target remote :1234'
```

### Docker

The multi-stage `Dockerfile` wraps QEMU and the toolchain.

```bash
# default target: build a no-HAL example and boot it under QEMU
docker build -t vaios:qemu .
docker run --rm vaios:qemu                       # FIFO_TEST
docker run --rm -e VAIOS_EXAMPLE=PERF_DEMO vaios:qemu

# interactive dev shell with the full toolchain (mount your tree)
docker build --target dev -t vaios:dev .
docker run --rm -it -v "$PWD:/project" vaios:dev
```

---

## 5. Run every layer at once

```bash
tools/run_all_tests.sh                 # every layer whose tools are present
tools/run_all_tests.sh host sitl       # only the named layers
PORT=/dev/ttyACM1 tools/run_all_tests.sh
```

Layers (each skips cleanly if its prerequisites are missing):

| Layer | What | Needs |
| ----- | ---- | ----- |
| `host` | host unit suite (`run_tests.sh`) | `gcc` |
| `coverage` | gcov report (`coverage.sh`) | `gcc`, `gcov` |
| `sitl` | UNIT_TESTS in Renode (`build_pil` + `renode.resc`) | `arm-none-eabi-gcc`, `renode` |
| `pitl` | hardware regression (`run_hw_tests.sh`) | ARM toolchain, `st-flash`, a board |

Exit code = number of layers that **failed** (skipped layers don't count).

---

## 6. Build options

Pass with `-D<OPTION>=<VALUE>` at configure time. Authoritative source:
top-level `CMakeLists.txt`.

| Option | Default | Purpose |
| ------ | ------- | ------- |
| `NAVHAL` | OFF | Build against the NavHAL submodule (required for real hardware / Renode) |
| `EXAMPLES` | OFF | Build the example selected by `VAIOS_EXAMPLE` |
| `VAIOS_EXAMPLE` | `""` | Which example to build (e.g. `FIFO_TEST`) — **required** to emit `examples/main` |
| `VAIOS_CONFIG_FILE` | `""` | Path to a user `vaios_config.h` override |
| `VAIOS_FPU` | ON | Use the hardware FPU |
| `VAIOS_MODULE_TERMINAL` | ON | Interactive command terminal |
| `VAIOS_MODULE_VFS` | OFF | VFS / FatFs layer (auto-forced ON by VFS examples) |
| `VAIOS_MODULE_SEMIHOSTING` | ON | Semihosting debug I/O |
| `VAIOS_MODULE_FIFO` | ON | SPSC/MPMC FIFO data structures |
| `VAIOS_MODULE_PERF` | ON | Kernel performance counters |

### NavHAL configuration

The NavHAL Kconfig is owned here as `navhal.config` and copied into
`extern/NavHAL/.config` at configure time, so the submodule stays pristine.
Edit `navhal.config` directly, or use NavHAL's menuconfig and save the result
back:

```bash
cd extern/NavHAL && python3 tools/kconfig.py --menuconfig
```

---

## Quick reference

```bash
# Host tests (no board, no toolchain)
bash tools/run_tests.sh

# Build + flash an example to a Nucleo
export srctree="$PWD/extern/NavHAL"
tools/flash.sh FIFO_TEST

# Boot an example in Renode (emulation)
cmake -S . -B build_renode -DNAVHAL=ON -DEXAMPLES=ON -DVAIOS_EXAMPLE=MULTI_TASK
cmake --build build_renode
renode --console --disable-xwt -e "include @tools/renode.resc"

# No-HAL smoke test in QEMU
tools/qemu_no_hal.sh

# Everything your machine can run
tools/run_all_tests.sh
```
