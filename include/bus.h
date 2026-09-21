#ifndef VAIOS_BUS_H
#define VAIOS_BUS_H

// Bus IPC subsystem: payload-agnostic publish/subscribe over a fixed block pool.
// Design: docs/plans/bus-subsystem.md. Each phase adds its declarations here as
// it lands; so far (B0/B1) that is the bus itself and its block-pool allocator,
// which later phases build messages from.
//
// Storage is caller-owned, like the peripheral-bus arbiter (periph_bus.h): the
// kernel keeps no object table and the data path never allocates.
//
//   V_BUS_POOL(ctl_pool, 64, 128);   // 128 blocks of 64 bytes, static
//   static v_bus_t ctl;
//   v_bus_init(&ctl, ctl_pool_blocks, ctl_pool_desc, 64, 128);
//
// Built only with VAIOS_MODULE_BUS.

#include "ipc.h" // VA_PASS / VA_FAIL
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define V_BUS_NIL 0xFFFFu // null block index; caps a pool at 65535 blocks

// Per-block descriptor, parallel to the block array. Private to the bus.
typedef struct {
  uint16_t next; // free list, or the next block of the same message
  uint8_t used;
} v_bus_desc_t;

typedef struct v_bus {
  // --- private ---
  uint8_t *blocks;
  v_bus_desc_t *desc;
  uint16_t block_size;
  uint16_t block_count;
  uint16_t free_head; // free stack, threaded through desc[].next
  uint16_t free_count;
} v_bus_t;

// Declare a bus's static storage: <name>_blocks (block_count x block_size
// bytes, word-aligned) and <name>_desc (block_count descriptors).
#define V_BUS_POOL(name, block_size, block_count)                              \
  static uint8_t name##_blocks[(block_size) * (block_count)]                   \
      __attribute__((aligned(4)));                                             \
  static v_bus_desc_t name##_desc[(block_count)]

// Wire caller storage into a bus, all blocks free. VA_FAIL on a NULL argument,
// a zero block size, or a block count of 0 or >= V_BUS_NIL.
int v_bus_init(v_bus_t *bus, void *blocks, v_bus_desc_t *desc,
               uint16_t block_size, uint16_t block_count);

// --- Block pool (bus internals; used by the message phases and tests) --------
// Take n blocks, all or nothing, chained through desc[].next and ending in
// V_BUS_NIL. Returns the head index, or V_BUS_NIL if fewer than n are free (or
// n is 0). O(n), independent of the pool size. Task or ISR context.
uint16_t v_bus_block_alloc(v_bus_t *bus, uint16_t n);
// Return a whole chain from v_bus_block_alloc. VA_FAIL, freeing nothing, if
// the chain is not a live allocation (bad index, or a block already free, e.g.
// a double free). Task or ISR context.
int v_bus_block_free(v_bus_t *bus, uint16_t head);
// Next block of a chain, or V_BUS_NIL.
uint16_t v_bus_block_next(const v_bus_t *bus, uint16_t idx);
// Payload bytes of a block (block_size of them).
uint8_t *v_bus_block_data(v_bus_t *bus, uint16_t idx);
uint16_t v_bus_free_blocks(const v_bus_t *bus);

// Invariant H1 (plan §4.2): the free list holds exactly free_count blocks, all
// marked free, with no cycle, and every other block is marked used. VA_PASS
// when consistent. Walks the pool without locking, so call it on a quiescent
// bus: tests and debug checks.
int v_bus_check(const v_bus_t *bus);

#ifdef __cplusplus
}
#endif

#endif // VAIOS_BUS_H
