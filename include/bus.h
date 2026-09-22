#ifndef VAIOS_BUS_H
#define VAIOS_BUS_H

// Bus IPC subsystem: payload-agnostic publish/subscribe over a fixed block
// pool. Design: docs/plans/bus-subsystem.md. Each phase adds its declarations
// here as it lands; so far: the bus and its block pool (B0/B1), topics with
// single-producer publish and polling pop (B2), unsubscribe, the overwrite
// policy and slow-subscriber recovery (B3), a zero-copy path for
// single-block messages (reserve/commit, peek/release), per-topic soft
// reservations (cfg.reserve), and pipe topics (cfg.pipe: a lock-free
// single-producer/single-consumer ring for hot point-to-point paths).
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
  uint8_t used;   // 0 free (pool), 1 in use, 2 reserved (v_bus_reserve,
                  // uncommitted), 3 idle in a topic's stash (cfg.reserve),
                  // 4 a slot of a pipe topic's ring
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
  // Soft reservation: blocks this topic keeps for itself. At declare they move
  // from the pool into the topic's own stash; its messages draw from the
  // stash first and return there, so another topic filling the pool can't
  // starve it — and the hot path skips the shared pool. "Soft": while idle,
  // they may be lent to a V_BUS_OVERWRITE topic when the pool is empty, and
  // are taken back by evicting that borrower's oldest messages when needed.
  uint16_t reserve;
  // Pipe: the caller promises ONE producer context and ONE subscriber, and
  // the topic becomes a lock-free ring of `reserve` single-block slots — the
  // bus's SPSC path, no critical sections, no ref counts, no pool traffic.
  // Same API; a second subscribe is refused, multi-block messages too.
  // V_BUS_DROP refuses when the ring is full (the producer never touches an
  // unread slot, so a peek stays valid); V_BUS_OVERWRITE overwrites the
  // oldest, readers detect it per slot (seqlock) and report it as `missed`.
  uint8_t pipe;
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
  uint16_t stash;       // idle reserved blocks, threaded through desc[].next
  uint16_t stash_count; // stash_count + lent <= cfg.reserve, always
  uint16_t lent;        // stash blocks currently lent to overwrite topics
  uint16_t borrowed;    // blocks this topic owes lenders (repaid on free)
  uint16_t pipe_wr;     // pipe: the slot the next message goes into
  uint32_t next_seq;    // pipe: written only by the producer (lock-free)
};

// A subscription: one reader's position in a topic. Caller storage, private.
// Each subscription belongs to one task: pop/unsubscribe on it don't race.
struct v_bus_sub {
  v_bus_topic_t *topic;
  v_bus_sub_t *next; // on topic->subs
  uint16_t cursor;   // next unread message, V_BUS_NIL when caught up
  uint16_t peeked;   // message held by v_bus_peek, V_BUS_NIL if none
  uint32_t expect;   // seq it expects next; a gap = messages it missed
};

// Bytes of bus header at the start of each message's first block.
#define V_BUS_HDR_SIZE 12u

// Status codes beyond VA_PASS / VA_FAIL.
#define V_BUS_EINVAL (-22)
#define V_BUS_EMSGSIZE (-90) // pop buffer smaller than the message
#define V_BUS_ESPLIT (-40)   // zero-copy call on a message wider than a block
#define V_BUS_EBUSY (-16)    // a peek is already held / a pipe's one subscriber
#define V_BUS_ESTALE (-116)  // overwrite pipe: the peeked slot was overwritten
                             // while viewed — discard what was read

// Wire caller storage into a bus, all blocks free. VA_FAIL on a NULL argument,
// a block count of 0 or >= V_BUS_NIL, or a block size that isn't a multiple
// of 4 holding at least the message header (V_BUS_HDR_SIZE).
int v_bus_init(v_bus_t *bus, void *blocks, v_bus_desc_t *desc,
               uint16_t block_size, uint16_t block_count);

// --- Topics and messages (B2) ------------------------------------------------
// Declare `topic` on `bus` under `name` (kept by pointer, must outlive the
// topic). `cfg` NULL = defaults (V_BUS_DROP, no reservation). V_BUS_EINVAL
// on a NULL argument or a name already declared; VA_FAIL if the pool can't
// cover cfg->reserve right now.
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
// Blocks come from the topic's reservation, then the shared pool; a
// V_BUS_OVERWRITE topic may then borrow other topics' idle reserved blocks,
// and finally evicts its own oldest messages until the new one fits
// (subscribers that hadn't read them skip ahead and see the gap as `missed`).
// A topic short of its own reservation takes lent blocks back by evicting the
// borrowers' oldest messages.
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

// --- Zero-copy path (single-block messages) ----------------------------------
// For hot paths: the payload is written and read in place in a pool block,
// no copies. Limited to messages that fit one block (block_size -
// V_BUS_HDR_SIZE bytes) — a longer one isn't contiguous; use publish/pop.
//
//   uint16_t t;
//   imu_t *s = v_bus_reserve(&imu, sizeof *s, &t);
//   if (s) { fill(s); v_bus_commit(&imu, t, sizeof *s); }
//
//   const imu_t *s; uint16_t len; uint32_t missed;
//   if (v_bus_peek(&est, (const void **)&s, &len, &missed) == VA_PASS) {
//     use(s); v_bus_release(&est);
//   }
//
// Reserve a block for up to `max_len` payload bytes and return where to write
// them (*ticket names the reservation), or NULL: max_len wider than a block,
// or no room (same drop/overwrite policy as v_bus_publish). Nobody sees it
// until v_bus_commit. Task or ISR context, one producer per topic.
void *v_bus_reserve(v_bus_topic_t *topic, uint16_t max_len, uint16_t *ticket);
// Publish a reservation with its final length (<= what was reserved). VA_PASS;
// V_BUS_EINVAL on a bad ticket or length.
int v_bus_commit(v_bus_topic_t *topic, uint16_t ticket, uint16_t len);
// Give a reservation back unpublished.
int v_bus_cancel(v_bus_topic_t *topic, uint16_t ticket);

// Point *data at the subscription's oldest unread message, in place; *len and
// (if non-NULL) *missed as for v_bus_pop. The message is pinned — overwrite
// won't evict it — until v_bus_release, so the view stays valid; keep it
// short, since an overwrite topic can't evict a pinned oldest message (its
// publishes drop meanwhile). One peek per subscription at a time. VA_PASS;
// VA_FAIL when nothing is pending; V_BUS_ESPLIT for a multi-block message
// (read it with v_bus_pop); V_BUS_EBUSY if a peek is already held.
int v_bus_peek(v_bus_sub_t *sub, const void **data, uint16_t *len,
               uint32_t *missed);
// Done with the peeked message: consume it, like a completed v_bus_pop. On a
// V_BUS_OVERWRITE pipe (the only case where the writer isn't held back),
// V_BUS_ESTALE says the slot was overwritten while it was being read.
int v_bus_release(v_bus_sub_t *sub);

// --- Pool state ---------------------------------------------------------------
// Blocks in the shared pool (not counting topics' reserved stashes).
uint16_t v_bus_free_blocks(const v_bus_t *bus);

// Invariant H1 (plan §4.2): the free list holds exactly free_count blocks, all
// marked free, with no cycle, and every other block is marked used; every
// topic's queue is a well-formed chain of live, still-owed messages that each
// subscription's cursor points into; each stash is well-formed and within its
// reservation; loans balance; and free + stashes + queued + open reservations
// == block_count (nothing leaked). VA_PASS when consistent. Walks the pool
// without locking, so call it on a quiescent bus: tests and debug checks.
int v_bus_check(const v_bus_t *bus);

#ifdef __cplusplus
}
#endif

#endif // VAIOS_BUS_H
