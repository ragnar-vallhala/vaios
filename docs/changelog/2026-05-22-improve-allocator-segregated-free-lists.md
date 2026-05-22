# Improve: Heap Allocator — O(1) Free and Segregated Free Lists

**Date:** 2026-05-22
**Severity:** Improvement — closed an ~11× small-malloc deficit vs FreeRTOS
**Files Modified:** `include/memory.h`, `kernel/memory.c`

## Problem

The allocator was O(n) on both paths and non-deterministic under fragmentation:

- `v_free()` walked the heap from the head on every call to find the block
  preceding the one being freed (needed for backward coalescing).
- `v_malloc()` linear-scanned every block from the head, first-fit.

Benchmarking (`malloc_64B` ~3000 cyc vs FreeRTOS ~270) showed small-block
allocation was the standout algorithmic gap.

## Fix

Two stages.

**Stage A — O(1) free.** The unused 16-byte-header padding slot became a
`prev` pointer (address-order predecessor). `v_free()` now coalesces backward
via `block->prev` and forward via the trailer address — no heap walk. Header
size unchanged.

**Stage B — segregated free lists.** The heap initialises as one big free
block (no "wilderness" region). Eight size-class free lists are indexed by
payload size; each free block carries its doubly-linked list pointers inside
its own (unused-while-free) payload, so the header stays 16 bytes. `v_malloc()`
looks up the size class — first-fit within it, else the head of any larger
class — instead of walking the whole heap. `v_free()` coalesces (splicing
neighbours off their lists) and files the merged block on its class.

## Result

`malloc` is O(1) for the fixed-size alloc/free patterns typical of an RTOS;
`free` stays O(1). The allocator went from an ~11× deficit to beating both
FreeRTOS and Zephyr on nearly every malloc/free benchmark. `free` is ~1.5×
slower than the old coalesce-only path (it now maintains the size-class lists)
but still best-in-class.

## Lesson

A flight allocator must be deterministic, not merely "usually fast." Segregated
free lists trade a few bytes of per-block bookkeeping (stored in the free
block's own payload, costing nothing) for bounded, predictable allocation.
