#ifndef VAIOS_BUS_H
#define VAIOS_BUS_H

// Bus IPC subsystem: payload-agnostic publish/subscribe over a fixed block pool.
// Design: docs/plans/bus-subsystem.md. Each phase adds its declarations here as
// it lands; so far: the bus and its block pool (B0/B1), topics with
// single-producer publish and polling pop (B2), and unsubscribe, the overwrite
// policy and slow-subscriber recovery (B3).
//
// Storage is caller-owned, like the peripheral-bus arbiter (periph_bus.h): the
// kernel keeps no object table and the data path never allocates.
//
//   V_BUS_POOL(ctl_pool, 64, 128);   // 128 blocks of 64 bytes, static
//   static v_bus_t ctl;
//   static v_bus_topic_t imu;
//   static v_bus_sub_t est;
//   v_bus_init(&ctl, ctl_pool_blocks, ctl_pool_desc, 64, 128);
//   v_bus_topic_declare(&ctl, &imu, "imu.raw", NULL);   // NULL: drop when full
//   v_bus_subscribe(&imu, &est);
//   v_bus_publish(&imu, &sample, sizeof sample);        // producer (task/ISR)
//   v_bus_pop(&est, &sample, sizeof sample, &len, &missed);   // consumer
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
  uint16_t next;  // free list, or the next block of the same message
  uint16_t epoch; // bumped each time the block is freed (stale-read guard)
  uint8_t used;
} v_bus_desc_t;

typedef struct v_bus_sub v_bus_sub_t;
typedef struct v_bus_topic v_bus_topic_t;

typedef struct v_bus {
  // --- private ---
  v_bus_topic_t *topics; // declared topics (names are unique per bus)
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

// What a topic does when the pool can't take a new message (plan §5.3).
typedef enum {
  V_BUS_DROP = 0,  // refuse the new one (commands: never lose an old one)
  V_BUS_OVERWRITE, // evict this topic's oldest (state: latest wins)
} v_bus_overflow_t;

typedef struct {
  v_bus_overflow_t overflow;
} v_bus_topic_cfg_t;

// A topic: an ordered queue of messages on one bus. Caller storage, private.
struct v_bus_topic {
  v_bus_t *bus;
  const char *name;
  v_bus_topic_t *next; // on bus->topics
  v_bus_sub_t *subs;
  v_bus_topic_cfg_t cfg;
  uint16_t head, tail; // oldest / newest live message (head block index)
  uint16_t nsubs;
  uint16_t blocks; // pool blocks held by queued messages
  uint32_t next_seq;
};

// A subscription: one reader's position in a topic. Caller storage, private.
// Each subscription belongs to one task: pop/unsubscribe on it don't race.
struct v_bus_sub {
  v_bus_topic_t *topic;
  v_bus_sub_t *next;  // on topic->subs
  uint16_t cursor;    // next unread message, V_BUS_NIL when caught up
  uint32_t expect;    // seq it expects next; a gap = messages it missed
};

// Bytes of bus header at the start of each message's first block.
#define V_BUS_HDR_SIZE 12u

// Status codes beyond VA_PASS / VA_FAIL.
#define V_BUS_EINVAL (-22)
#define V_BUS_EMSGSIZE (-90) // pop buffer smaller than the message

// Wire caller storage into a bus, all blocks free. VA_FAIL on a NULL argument,
// a block count of 0 or >= V_BUS_NIL, or a block size that isn't a multiple
// of 4 holding at least the message header (V_BUS_HDR_SIZE).
int v_bus_init(v_bus_t *bus, void *blocks, v_bus_desc_t *desc,
               uint16_t block_size, uint16_t block_count);

// --- Topics and messages (B2) ------------------------------------------------
// Declare `topic` on `bus` under `name` (kept by pointer, must outlive the
// topic). `cfg` NULL = defaults (V_BUS_DROP). V_BUS_EINVAL on a NULL argument
// or a name already declared.
int v_bus_topic_declare(v_bus_t *bus, v_bus_topic_t *topic, const char *name,
                        const v_bus_topic_cfg_t *cfg);
// Attach `sub` to `topic`. It sees only messages published from now on.
int v_bus_subscribe(v_bus_topic_t *topic, v_bus_sub_t *sub);
// Detach `sub`, releasing every message it still owed (plan H6): anything no
// other subscriber is waiting for goes back to the pool. Task context.
int v_bus_unsubscribe(v_bus_sub_t *sub);

// Publish `len` bytes (copied in). Task or ISR context; ONE producer per topic
// for now (multi-producer lands with B5). VA_PASS, also when nobody is
// subscribed (nothing is kept then); VA_FAIL when the pool can't hold it right
// now and the topic is V_BUS_DROP (or evicting its whole queue wouldn't make
// room); V_BUS_EINVAL on bad arguments or a message the pool could never hold.
// A V_BUS_OVERWRITE topic evicts its oldest messages until the new one fits;
// subscribers that hadn't read them skip ahead and see the gap as `missed`.
int v_bus_publish(v_bus_topic_t *topic, const void *payload, uint16_t len);
// Take the subscription's oldest unread message into `out` (`cap` bytes),
// setting *out_len and, if `missed` is non-NULL, how many messages this
// subscription lost to overwrite since its last pop. VA_PASS; VA_FAIL when
// nothing is pending; V_BUS_EMSGSIZE when it doesn't fit (*out_len says how
// big it is; it stays unread). Polling only for now (blocking lands with B6).
// A message's blocks go back to the pool once every subscriber that was
// attached at publish time has read it. A message evicted while being copied
// out is detected (block epoch) and the next one is read instead.
int v_bus_pop(v_bus_sub_t *sub, void *out, uint16_t cap, uint16_t *out_len,
              uint32_t *missed);

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
// marked free, with no cycle, and every other block is marked used; and every
// topic's queue is a well-formed chain of live, still-owed messages that each
// subscription's cursor points into. VA_PASS when consistent. Walks the pool without locking, so call it on a quiescent
// bus: tests and debug checks.
int v_bus_check(const v_bus_t *bus);

#ifdef __cplusplus
}
#endif

#endif // VAIOS_BUS_H
