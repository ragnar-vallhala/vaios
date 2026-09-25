# Bus IPC benchmark (plan B8)

Measured numbers, how they were taken, and what they settle. The benchmark is
`examples/benchmark/bench_bus.c` — part of the committed suite rather than an
ad-hoc script, so a future change can be compared against the same four cases.

## The four cases

| Case | What it times |
|---|---|
| `BUS copy publish+pop` | one publish plus one pop of a single-block message — the copying path a user task gets through `SYS_bus_send`/`SYS_bus_recv` |
| `BUS zero-copy reserve+peek` | `v_bus_reserve`/`commit` plus `v_bus_peek`/`release`: the payload never moves |
| `BUS pipe publish+pop` | the same round trip on a pipe topic (the lock-free SPSC ring) |
| `BUS copy under a full pool` | the copying path again with the pool saturated and a second reader that never reads, so every publish must evict before it can allocate |

**The max is the point.** A mean tells you throughput; the worst case tells you
whether a 1 kHz control loop makes its deadline.

## Results — STM32F401RE, 84 MHz, 2026-09-25

200 round trips per case, DWT cycle counter, hard-float, `-O2`, ART accelerator
**off** (a NavHAL clock-init gap, fix upstream). One cycle = 11.9 ns.

| Case | min | mean | max | mean µs | max µs |
|---|---|---|---|---|---|
| pipe publish+pop | 563 | 566 | 1355 | 6.7 | 16.1 |
| zero-copy reserve+peek | 717 | 720 | 1508 | 8.6 | 18.0 |
| copy publish+pop | 833 | 841 | 1637 | 10.0 | 19.5 |
| copy under a full pool | 1001 | 1008 | 1800 | 12.0 | 21.4 |

### What these say

**Eviction under a full pool costs about 20%, not a cliff.** Mean goes 841 →
1008 cycles (+167, ≈2 µs) and the max 1637 → 1800 (+10%). That answers the open
question about `alloc_msg`'s reclaim/evict loop being the one critical section
whose length grows with pool pressure: it does grow, but by two microseconds, and
**it does not need to be made incremental.** Revisit only if a future change
lengthens that loop.

**The worst case is interrupt intrusion, not allocator variance.** Every case's
max sits near 2× its min while min and mean are within 1% of each other. A 1 kHz
SysTick inside a ~10 µs measurement window lands in roughly one sample in a
hundred, which is exactly the shape seen. So the bus's own worst case is close to
its mean; the max quoted here includes a tick ISR.

**Ordering is as designed.** Pipe < zero copy < copy < copy-under-load. The pipe
is the fast path because it takes no critical section at all on the hot path; zero
copy beats copying because the payload never moves.

**Budget:** at a 1 kHz control loop (1000 µs), the worst case measured here is
2.1% of the period.

These are not comparable to the ad-hoc figures taken while the zero-copy and pipe
paths were being written: that harness measured different call sequences, and the
publish path has since gained the B7 statistics counters. Treat this table as the
baseline.

## Running it

```sh
export srctree="$PWD/extern/NavHAL"
cmake -S . -B build_bench -DNAVHAL=ON -DEXAMPLES=ON \
      -DVAIOS_EXAMPLE=BENCHMARK -DVAIOS_MODULE_BUS=ON \
      -DVAIOS_BENCH_ONLY_BUS=ON
# This example's statics plus the default 88 KB heap do not fit in the F401's
# 96 KB of SRAM — see below. Give it a smaller heap:
echo 'CONFIG_HEAP_SIZE=0x4000' >> build_bench/.config
cmake --build build_bench -j
```

`-DVAIOS_BENCH_ONLY_BUS=ON` skips the FPU/DMA/task/IPC/memory/stress suites.

Reading the results needs no serial port: they are in `bus_bench_cycles[4][3]`
(min/mean/max per case) and in `g_results[BM_BUS_*]`, so a debugger can read them
straight out of RAM — which is how the table above was taken, on a board whose
ST-Link clone has no VCP:

```sh
tools/pitl_run.sh --kmsg build_bench 12     # flash + run
st-util --no-reset -p 4242 &
gdb-multiarch -batch -ex 'target extended-remote :4242' -ex interrupt \
  -ex 'print bus_bench_cycles' build_bench/examples/benchmark/benchmark
```

## Two things that had to be fixed to get here

Both were pre-existing, and worth recording because they made the benchmark look
like it "didn't start":

1. **The heap did not fit in RAM, and failed silently.** `_heap_start` sits after
   `.bss`; with this example's ~36 KB of statics and the default `HEAP_SIZE` of
   88 KB, the arena ended past the top of SRAM and `v_heap_memory_init`'s
   `memset` walked off the end — BusFault, escalated to HardFault, at boot.
   `v_heap_memory_init` now checks the arena against `v_port_ptr_is_ram` and
   panics with a message naming the size and address instead.
2. **Nothing before `scheduler_start` could be seen.** With `BUFFERED_LOGGING=1`,
   `v_log` writes into a double buffer that only reaches the console when
   `v_log_flush` runs — and its callers run under the scheduler. So every early
   bring-up failure, including the `log an error then while(1)` paths in
   `v_system_init`, produced a dead board with no output. `v_log` now flushes
   inline when the scheduler is not running.

An earlier version of this document blamed Renode for the benchmark not starting.
That was wrong: it was failing the same way everywhere, and nothing could say so.

## Still to do

- Re-baseline once NavHAL ships the ART accelerator enabled; expect these to drop
  materially, and state the flash/cache configuration next to the new numbers.
- Take the same four cases on an M7 part (F767) for comparison, where the cache
  and the wider bus change the shape.
