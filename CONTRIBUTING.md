# Contributing to VaiOS

Thanks for your interest in VaiOS. This document covers how to file issues, set
up a dev environment, and submit changes.

By participating you agree to abide by our [Code of Conduct](CODE_OF_CONDUCT.md).

## Where to file what

| Kind | Where |
|---|---|
| Reproducible bug, concrete task | [Issues](https://github.com/ragnar-vallhala/vaios/issues) |
| Feature idea, design discussion, "how do I…" | [Discussions](https://github.com/ragnar-vallhala/vaios/discussions) |
| Security report | Email the maintainer privately at ragnarvallhala865@gmail.com; please do not open a public issue |
| Code change | Pull request — see below |

Search existing issues and discussions before opening a new one.

## Development environment

You'll need:

* `arm-none-eabi-gcc` toolchain (`gcc-arm-none-eabi`, `binutils-arm-none-eabi`,
  `libnewlib-arm-none-eabi` on Debian/Ubuntu) — for the Cortex-M4 / STM32F4
  target.
* `cmake` ≥ 3.20
* `make` or `ninja`
* For host unit tests, the system `gcc` is sufficient — no cross-compiler needed.

Optional:

* `st-flash` (stlink-tools) for flashing the Nucleo-F401RE.
* Python 3 with `kconfiglib` (`pip install kconfiglib`) — only if you want to
  re-run NavHAL's `menuconfig`; see [NavHAL configuration](#navhal-configuration).
* Docker, for the bundled no-HAL QEMU smoke build (`docker build -t vaios-arm-qemu .`).

The repo pulls NavHAL in as a submodule, so clone with submodules:

```bash
git clone --recurse-submodules https://github.com/ragnar-vallhala/vaios.git
# or, in an existing clone:
git submodule update --init --recursive
```

## Building

### An example (on real hardware)

```bash
cmake -B build -DNAVHAL=ON -DEXAMPLES=ON -DVAIOS_EXAMPLE=FIFO_TEST
cmake --build build -j
cmake --build build --target flash    # if st-flash is installed and the board is connected
```

Example names are the `VAIOS_EXAMPLE` branches in
[`examples/CMakeLists.txt`](examples/CMakeLists.txt) — e.g. `FIFO_TEST`,
`IPC_TEST`, `PRIORITY_INVERSION`, `HEAP_ALLOCATOR`, `MULTI_TASK`, `TERMINAL`,
`UART`, `BENCHMARK`. See the [README](README.md#build-and-flash-stm32-hardware)
for the full flash + UART-capture flow and the build-option table.

### NavHAL configuration

NavHAL's Kconfig (`.config`) is owned in this repo as `navhal.config` and copied
into `extern/NavHAL/.config` at configure time, so the submodule stays pristine.
Edit `navhal.config` directly, or run NavHAL's interactive menuconfig and save
the result back:

```bash
cd extern/NavHAL && python3 tools/kconfig.py --menuconfig
```

## Testing

Three layers, each runnable from one script. Exit codes are non-zero on any
failure, so all three are CI-friendly.

| Layer | Command | What it does |
| ----- | ------- | ------------ |
| Host unit tests | `bash tools/run_tests.sh` | Builds and runs the host-native suites under `tests/` with `gcc` (no toolchain, no board). NavHAL headers are stubbed. |
| Hardware regression | `bash tools/run_hw_tests.sh` | Flashes a curated set of examples to a connected Nucleo, captures UART, and greps for required PASS lines. Needs the ARM toolchain, `st-flash`, and `/dev/ttyACM0` (override with `PORT=...`). |
| CI | `.github/workflows/ci.yml` | Runs the host suite on every push and PR (Ubuntu runner). |

Run the host suite before opening a PR — it's what CI gates on:

```bash
bash tools/run_tests.sh
```

## Commit message format

[Conventional Commits](https://www.conventionalcommits.org/). Subject line:

```
<type>[(<scope>)][!]: <subject>
```

| Field | Values |
|---|---|
| `type` | `feat fix docs style refactor perf test build ci chore revert` |
| `scope` | Optional. Affected area, lowercase, e.g. `(ipc)`, `(kernel)`, `(task)`, `(port)`, `(spsc)`, `(navhal)`. |
| `!` | Optional. Marks a breaking change. |
| subject | Imperative mood, no trailing period; keep the whole line ≲ 72 chars. |

Body (optional) goes after a blank line. Explain *why*, not *what* — the diff
shows the what.

Examples (from this repo's history):

```
feat(task): add task names + drift-free periodic delay
fix(ipc): exit critical before yielding (give<->take race)
fix(kernel): heap honours app HEAP_SIZE + SysTick drives NavHAL timebase
refactor(port): route all kernel hardware access through a port facade
build: bump NavHAL submodule to stable 0.2.1
test(perf): cover task_snapshot_list, SPSC overwrite drops, stack edges
```

## Pull request workflow

1. **Branch off `main`** with a descriptive name: `git checkout -b feat/spsc-overflow-counter`.
2. **Make focused commits** — one logical change per commit; rebase to clean up
   WIP before opening the PR.
3. **Run the host tests**: `bash tools/run_tests.sh`. If your change touches the
   port, scheduler, or IPC and you have a board, also run
   `bash tools/run_hw_tests.sh`.
4. **Push and open a PR**: `git push -u origin <branch>` then `gh pr create --fill`.
5. **Wait for CI** — the `Host unit tests` job must be green before merge.
6. **Address review comments** as additional commits.

Every change lands through a PR against `main`; please don't push directly to it.

## Code style

* C11 throughout; the kernel and port build at `-O2`.
* Public API: `v_<subsystem>_<verb>(...)` (e.g. `v_mutex_lock`, `v_malloc`,
  `v_task_create`). Keep new public symbols in `include/`.
* **Route all hardware access through the port layer** (`portable/cortex-m4/`) —
  the kernel must not touch registers or NavHAL directly. Architecture-specific
  ASM and atomics live in `portable/<arch>/`.
* No new global mutable state without justification in the commit body.
* Avoid drive-by reformatting in functional commits — separate cleanup commits.
* Match the surrounding file's style.

## Adding a new example

1. Add `examples/<name>.c`.
2. Add a `VAIOS_EXAMPLE` branch for it in
   [`examples/CMakeLists.txt`](examples/CMakeLists.txt), mirroring an existing
   `elseif(VAIOS_EXAMPLE STREQUAL "...")` block.
3. Build it with `-DEXAMPLES=ON -DVAIOS_EXAMPLE=<NAME>` to confirm it links.

## Adding a new port (new MCU / architecture)

Ports live under `portable/<arch>/` and expose a fixed facade to the kernel —
see `portable/cortex-m4/` (`port.c`, `port.h`, `atomic.h`, plus the PendSV /
SVCall / BASEPRI plumbing) and the `portable/avr/` stub. A new port adds a
`portable/<arch>/` directory implementing that facade and wires it into
`portable/CMakeLists.txt`; the kernel itself stays architecture-agnostic.

## Licensing of contributions

By submitting a contribution, you agree that your work is licensed under the
[Apache License 2.0](LICENSE.md). Apache 2.0 Section 5 makes this implicit for
inbound contributions; no separate CLA is required.

Per-file header for new source files (copy from an existing file):

```c
/*
 * Copyright (C) 2025 Ashutosh Vishwakarma
 * Author: <your name>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * ...
 */
```
