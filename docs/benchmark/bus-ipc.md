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

## Results — NAVIXSM-F401RE (Cortex-M4), 84 MHz, 2026-09-29

200 round trips per case, DWT cycle counter, hard-float, `-O2`, NavHAL 0.3.8,
ART accelerator **on** (`FLASH_ACR = 0x702`: 2 wait states, prefetch, I-cache and
D-cache — read out of the running board, not assumed). One cycle = 11.9 ns.

| Case | min | mean | max | mean µs | max µs |
|---|---|---|---|---|---|
| pipe publish+pop | 432 | 432 | 477 | 5.1 | 5.7 |
| zero-copy reserve+peek | 596 | 604 | 1358 | 7.2 | 16.2 |
| copy publish+pop | 667 | 715 | 1440 | 8.5 | 17.1 |
| copy under a full pool | 857 | 864 | 1564 | 10.3 | 18.6 |

### What these say

**The ART accelerator is worth 14–24% of the mean.** Against the ART-off
baseline below: pipe 566 → 432 (−24%), zero copy 720 → 604 (−16%), copy 841 →
715 (−15%), copy under load 1008 → 864 (−14%). The pipe gains most, which is
what you would expect — it is the path with the least memory traffic, so it is
the one most dominated by instruction fetch.

**Eviction under a full pool still costs about 20%, and still needs no rework.**
Mean goes 715 → 864 (+149 cycles, ≈1.8 µs) and the max 1440 → 1564 (+9%). That is
the same shape as before ART, and the same answer for `alloc_msg`'s reclaim/evict
loop: the one critical section whose length grows with pool pressure grows by
under two microseconds. Revisit only if a future change lengthens that loop.

**The worst case is still interrupt intrusion, not allocator variance.** Three
of the four cases keep a max near 2× their min while min and mean sit within a
few percent of each other — a 1 kHz SysTick landing inside the measurement
window. The pipe no longer shows it: at 5.1 µs per round trip, 200 samples
expect about one tick hit, and this run caught none. So its 477-cycle max is the
bus's own worst case, uncontaminated; the other three maxima include a tick ISR.

**It reproduces to the cycle.** Two runs from reset returned byte-identical
numbers, maxima included. The tick lands at the same phase each time because the
benchmark starts a fixed distance after reset, so these are not averages to be
taken loosely — a change that moves them moved something real.

**Budget:** at a 1 kHz control loop (1000 µs), the worst case measured here is
1.9% of the period.

### Previous baseline — ART off, NavHAL 0.3.2, 2026-09-25

Kept for the delta. Same harness, same board, ART disabled because NavHAL did not
yet set `FLASH_ACR` (fixed in 0.3.8).

| Case | min | mean | max |
|---|---|---|---|
| pipe publish+pop | 563 | 566 | 1355 |
| zero-copy reserve+peek | 717 | 720 | 1508 |
| copy publish+pop | 833 | 841 | 1637 |
| copy under a full pool | 1001 | 1008 | 1800 |

Neither table is comparable to the ad-hoc figures taken while the zero-copy and
pipe paths were being written: that harness measured different call sequences,
and the publish path has since gained the B7 statistics counters.

## Results — Nucleo-F767ZI (Cortex-M7), 84 MHz, 2026-09-29

The same four cases on an M7, for the comparison the table above could not make.
Same harness, same 200 round trips, same DWT counter.

**The clock is the same 84 MHz**, not the F767's 216 MHz maximum — that is what
NavHAL's default configuration gives this board, and it is read out of the
SysTick reload (`0x1481f` → 83999+1 → 84.000 MHz at a 1000 µs tick), not
assumed. One cycle is 11.90 ns on both parts. So this is an architecture
comparison at matched clock, not a "which board is faster" one, which is the
more useful question anyway.

L1 caches and branch prediction are on: `CCR = 0x00070200`, bits 16/17/18, read
off the running board.

| Case | min | mean | max | mean µs | max µs | mean vs M4 |
|---|---|---|---|---|---|---|
| pipe publish+pop | 316 | 327 | 402 | 3.9 | 4.8 | −24% |
| zero-copy reserve+peek | 497 | 513 | 1108 | 6.1 | 13.2 | −15% |
| copy publish+pop | 567 | 591 | 1189 | 7.0 | 14.2 | −17% |
| copy under a full pool | 714 | 743 | 1340 | 8.8 | 16.0 | −14% |

### What these say

**The M7 buys 14–24% at the same clock**, in the same order the ART did on the
M4: the pipe gains most and the full pool least. Both accelerate the same thing
— getting instructions and data to the core — so the case with the least memory
traffic keeps gaining the most.

**Eviction still costs ~150 cycles, on either core.** The increment from `copy`
to `copy under a full pool` is +149 cycles on the M4 and +152 on the M7 — the
same absolute cost, despite everything else getting 14–24% cheaper. Neither the
ART nor the M7's caches touch it, which says `alloc_msg`'s reclaim loop is
bound by dependent loads walking the free list, not by fetch. That is also the
clearest statement yet that the loop needs no rework: the one critical section
that grows with pool pressure costs about 150 cycles wherever you run it.

**The M7 is faster but less repeatable.** Two runs from reset on the F401 with
ART returned byte-identical numbers, maxima included. On the M7 they do not:

    run 1   copy 567/591/1189   zc 497/513/1108   pipe 316/327/402   pool 714/743/1340
    run 2   copy 566/586/1173   zc 497/515/1165   pipe 315/328/402   pool 719/743/1344

Means agree within about 1%, but no column is exactly reproducible, because a
cache's contents depend on everything that ran before. For a control loop that
is the real trade: the M7 gives you a lower worst case and a slightly fuzzier
one. Quote the max, not the mean.

**Budget:** worst case 16.0 µs, 1.6% of a 1 kHz period, against 1.9% on the M4.

### Reproducing it

The stock F767 defconfig is not enough — it leaves out the DWT the benchmark
reads and builds soft-float, which would confound the comparison with an ABI
change. Copy it and add:

```sh
cp extern/NavHAL/cmake/defconfigs/cortex-m7_stm32f7_nucleo_f767zi.defconfig f767_bench.config
printf 'CONFIG_DRV_DWT=y\nCONFIG_USE_FPU=y\nCONFIG_DRV_FPU=y\n' >> f767_bench.config
cmake -S . -B build_bench7 -DNAVHAL=ON -DEXAMPLES=ON \
      -DVAIOS_EXAMPLE=BENCHMARK -DVAIOS_MODULE_BUS=ON -DVAIOS_BENCH_ONLY_BUS=ON \
      -DNAVHAL_CONFIG_FILE="$PWD/f767_bench.config"
cmake --build build_bench7 -j
```

No `CONFIG_HEAP_SIZE` override is needed: the F767's 512 KB of SRAM takes the
default heap that the F401's 96 KB could not. Flash and read exactly as above,
with `USB_LOC` naming the F767's probe — `probe_target_from_build` picks
`target/stm32f7x.cfg` from the build, so nothing else changes.

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
. tools/lib/probe.sh                    # USB_LOC picks the probe; default 3-2
probe_flash build_bench/examples/benchmark/benchmark   # programs AND starts it
sleep 15                                # let all four cases finish
oocd=$(probe_gdbserver); sleep 2
gdb-multiarch -batch -ex 'target extended-remote :3333' -ex interrupt \
  -ex 'print bus_bench_cycles' build_bench/examples/benchmark/benchmark
kill "$oocd"
```

`tools/pitl_run.sh --kmsg` does not work on this build: the benchmark does not
compile in the kmsg ring, so there is no `kmsg_ring` to read. Read the array.

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
