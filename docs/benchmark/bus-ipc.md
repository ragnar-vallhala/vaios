# Bus IPC benchmark (plan B8)

What it measures, how to run it, and what the numbers may and may not be used
for. The benchmark itself is `examples/benchmark/bench_bus.c`, part of the
committed suite rather than an ad-hoc script, so a future change can be compared
against the same four cases.

## The four cases

| Case | What it times |
|---|---|
| `BUS copy publish+pop` | one publish plus one pop of a single-block message — the copying path a user task gets through `SYS_bus_send`/`SYS_bus_recv` |
| `BUS zero-copy reserve+peek` | `v_bus_reserve`/`commit` plus `v_bus_peek`/`release`: the payload never moves |
| `BUS pipe publish+pop` | the same round trip on a pipe topic (the lock-free SPSC ring) |
| `BUS copy under a full pool` | the copying path again with the pool saturated and a second reader that never reads, so every publish must evict before it can allocate |

Each reports **min / mean / max cycles** per round trip, and records the max in
the result table's `detail` field.

**The max is the point.** A mean tells you throughput; the worst case tells you
whether a 1 kHz control loop makes its deadline. The fourth case exists because
that is when the allocator does its most work — reclaim, then eviction — and it
is the figure a realtime budget has to survive, not the idle one.

Cycles come from the DWT counter, so the benchmark calls `v_perf_init()` first:
`v_init()` alone does not arm it, and without that every reading is zero.

## Running it

```sh
export srctree="$PWD/extern/NavHAL"
cmake -S . -B build_bench -DNAVHAL=ON -DEXAMPLES=ON \
      -DVAIOS_EXAMPLE=BENCHMARK -DVAIOS_MODULE_BUS=ON \
      -DCMAKE_C_FLAGS="-DVAIOS_BENCH_ONLY_BUS=1"
cmake --build build_bench -j
```

`-DVAIOS_BENCH_ONLY_BUS=1` skips the FPU/DMA/task/IPC/memory/stress suites, which
take far longer than one emulator run is worth.

Then flash it and read the UART (`tools/flash.sh`, `tools/run_hw_tests.sh` shows
the serial-port selection this repo uses with several probes attached).

## Status: the numbers are not committed yet

Two honest caveats, both about the measurement rather than the code:

1. **The benchmark example does not currently run under Renode.** It is a
   bare-`main` program, and in the emulator it stops with `systick_count` at 2
   and no UART output — before any benchmark runs. Every *scheduler-based*
   example (`56_bus_ipc.c`, `57_bus_user.c`) runs there fine, so this is
   specific to the benchmark example's startup, and it predates B8. Until that is
   fixed, these numbers come from hardware.

2. **The board's flash accelerator is off.** Earlier bus measurements on the F401
   read roughly 2x their Renode equivalents because NavHAL's clock init leaves the
   ART accelerator disabled. That fix is upstream. Numbers taken before it lands
   would be recorded against a configuration nobody ships, so the table below is
   deliberately left as a baseline to beat, not a result.

### Baseline to beat (ad-hoc, pre-B8, single-block messages)

Measured by hand while the zero-copy and pipe paths were built, on Renode and on
an F401 with the accelerator off. Kept here only so a regression is visible; it
is not a B8 report.

| Path | Renode cycles | board cycles | board µs |
|---|---|---|---|
| SPSC zero copy (raw ring, for comparison) | 100 | 180 | 2.14 |
| pipe zero copy | 175 | 361 | 4.30 |
| reserved topic, zero copy | 291 | 585 | 6.96 |
| pool topic, zero copy | 314 | 647 | 7.70 |
| SPSC copy (raw ring, for comparison) | 318 | 684 | 8.14 |
| pipe copy | 346 | 734 | 8.74 |

What it already tells us: a pipe is about 2x a raw SPSC ring while keeping named
topics, `missed` accounting and fd access; bus zero copy beats an SPSC ring that
copies; and fan-out to three readers is cheaper on the bus than three rings.

### To finish B8

- Re-baseline on hardware once the NavHAL accelerator fix lands, and state the
  clock and cache configuration next to the numbers.
- Add the under-load max from `BUS copy under a full pool`, which is the figure
  the flight loop's budget is built from.
- Either fix the benchmark example's startup under Renode or record that this
  suite is hardware-only, so nobody expects SITL to produce it.
