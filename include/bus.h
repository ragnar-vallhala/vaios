#ifndef VAIOS_BUS_H
#define VAIOS_BUS_H

// Bus IPC subsystem: payload-agnostic publish/subscribe over a fixed block
// pool. Design: docs/plans/bus-subsystem.md. Each phase adds its declarations
// here as it lands; so far: the bus and its block pool (B0/B1), topics with
// single-producer publish and polling pop (B2), unsubscribe, the overwrite
// policy and slow-subscriber recovery (B3), a zero-copy path for
// single-block messages (reserve/commit, peek/release), per-topic soft
// reservations (cfg.reserve), and pipe topics (cfg.pipe: a lock-free
// single-producer/single-consumer ring for hot point-to-point paths), and the
// fd API unprivileged tasks reach a topic through (B9), and blocking recv so a
// reader sleeps instead of polling (B6).
//
// Storage is caller-owned, like the peripheral-bus arbiter (periph_bus.h): the
// kernel keeps no object table and the data path never allocates.
//
// The API below is for PRIVILEGED callers: kernel code, ISRs (the
// single-producer publish path is meant for them) and privileged tasks. The
// bus, its topics and the pool are kernel memory, and the critical sections are
// BASEPRI writes an unprivileged task's MSR silently skips — so under
// VAIOS_MPU_USER_SEPARATION a user task calling these would fault on the first
// block it touched, without even being atomic.
//
// Unprivileged tasks use the fd API at the bottom of this file instead
// (v_bus_open / v_bus_send / v_bus_recv): a topic opened by name, payloads
// copied in and out through syscalls. The zero-copy calls stay privileged —
// they hand out a pointer INTO the shared pool, so exposing them would grant a
// task every topic's messages; a user-facing zero-copy path means a pipe whose
// ring is mapped to that one task.
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

#include "ipc.h" // VA_PASS / VA_FAIL, SemaphoreHandle_t
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
  // Statistics (B7). Bumped inside the critical sections that already own the
  // state they describe, so they cost a increment and no extra locking. Read
  // with v_bus_topic_stats; best-effort across counters by design (§9).
  uint32_t published; // messages made visible on this topic
  uint32_t dropped;   // publishes refused because the pool was full
  uint32_t evicted;   // messages overwritten out from under a reader
};

// A subscription: one reader's position in a topic. Caller storage, private.
// Each subscription belongs to one task: pop/unsubscribe on it don't race.
struct v_bus_sub {
  v_bus_topic_t *topic;
  v_bus_sub_t *next; // on topic->subs
  uint16_t cursor;   // next unread message, V_BUS_NIL when caught up
  uint16_t peeked;   // message held by v_bus_peek, V_BUS_NIL if none
  uint32_t expect;   // seq it expects next; a gap = messages it missed
  // Blocking mode (B6): when armed, publish signals this so a reader can sleep
  // instead of polling. 0 = polling only. Armed per subscription, so one
  // reader blocking costs nothing to the others.
  SemaphoreHandle_t notify;
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

// --- User access: topics on the fd table (VAIOS_DEVFS) ------------------------
// A privileged init declares the topics; a task opens one by name and talks to
// it through syscalls, so it never touches kernel memory:
//
//   int fd = v_bus_open("imu.raw", V_BUS_RD);
//   v_bus_rx_t rx = {.buf = &sample, .cap = sizeof sample};
//   if (v_bus_recv(fd, &rx) == VA_PASS) use(&sample, rx.len, rx.missed);
//   v_file_close(fd);
//
// Polling only for now (blocking pop is phase B6), and one subscription per
// open handle (VAIOS_BUS_MAX_OPEN of them, in kernel RAM, released by close —
// so a task that exits mid-stream drops its claim on queued messages).
#define V_BUS_RD 0x1 // subscribe: this handle can receive
#define V_BUS_WR 0x2 // this handle can publish

// v_bus_recv's in/out block: `buf`/`cap` in, `len`/`missed` out.
typedef struct {
  void *buf;
  uint16_t cap;
  uint16_t len;
  uint32_t missed;
} v_bus_rx_t;

#if VAIOS_DEVFS
// Open a declared topic by name (searched across every initialised bus).
// Returns an fd (>= 0), or a negative error: V_BUS_EINVAL for an unknown name
// or no direction, V_BUS_EBUSY when no handle or descriptor is free (or the
// topic is a pipe that already has its one reader). Never VA_FAIL, which would
// be indistinguishable from fd 0. Close it with v_file_close.
int v_bus_open(const char *name, int flags);
// Publish through a V_BUS_WR handle: as v_bus_publish (VA_PASS, VA_FAIL when
// the pool is full and the topic drops, V_BUS_EINVAL on bad arguments).
int v_bus_send(int fd, const void *payload, uint16_t len);
// Take this handle's oldest unread message into rx->buf (rx->cap bytes),
// setting rx->len and rx->missed: as v_bus_pop, including V_BUS_EMSGSIZE when
// it doesn't fit (rx->len says how big it is; the message stays unread).
int v_bus_recv(int fd, v_bus_rx_t *rx);
// As v_bus_recv, but waits up to `ticks` for a message instead of reporting the
// topic empty. ticks == 0 is exactly v_bus_recv. Returns VA_PASS, VA_FAIL on
// timeout, or the same errors v_bus_recv reports.
//
// Sleeping, not spinning: the handle's subscription carries a semaphore that
// publish signals (from an ISR too), and this waits on it and re-reads. The
// signal is a HINT, not a promise: it banks one wake, and a message can be
// evicted between the signal and the reader running, so a wake with nothing to
// read costs another turn of the loop and never a missed message. The
// wait and the read are separate steps on purpose — a blocked reader parks with
// NO pointer of its own left in kernel state, so a task that dies while waiting
// cannot leave the kernel writing into a stack that is gone.
int v_bus_recv_wait(int fd, v_bus_rx_t *rx, uint32_t ticks);
// Wait for this handle to have something to read, without reading it: VA_PASS
// when a message is (or became) available, VA_FAIL on timeout. v_bus_recv_wait
// is this plus v_bus_recv, and is what callers normally want.
int v_bus_wait(int fd, uint32_t ticks);
#endif

// --- Snapshotter (B7) ---------------------------------------------------------
// A best-effort recorder: it subscribes to a topic like any other consumer, so
// it takes part in reclaim and can never pin a message forever, and copies what
// it reads to a file through the VFS.
//
// There is no hidden task. The application runs it from its OWN lowest-priority
// task — v_bus_snapshot_pump() moves up to `max` messages and returns — because
// the priority at which recording happens is a flight decision, not the bus's.
// Realtime consumers always come first: a pump that falls behind loses messages
// (counted), it never delays a publisher or another reader.
//
// File format, one record per message: uint16 len, uint16 zero, uint32 missed,
// then len payload bytes. `missed` is the gap the bus reported before this
// message, so a reader can see where the recording lost data.
#if VAIOS_MODULE_VFS
typedef struct {
  v_bus_topic_t *topic;
  v_bus_sub_t sub;    // its own subscription: reclaim treats it like a reader
  int file;           // vfs handle, negative when not recording
  uint32_t written;   // messages written
  uint32_t failed;    // messages the filesystem refused (best effort: kept going)
  uint32_t missed;    // messages the bus dropped under it
} v_bus_snap_t;

// Open `path` and subscribe. VA_PASS, V_BUS_EINVAL, or the filesystem's error.
int v_bus_snapshot_start(v_bus_snap_t *snap, v_bus_topic_t *topic,
                         const char *path);
// Move up to `max` messages to the file. Returns the number written (>= 0), or
// V_BUS_EINVAL. Buffer size is the bus's block payload, so a message that does
// not fit one block is counted as failed rather than truncated.
int v_bus_snapshot_pump(v_bus_snap_t *snap, uint32_t max);
// Unsubscribe and close. Always safe to call twice.
int v_bus_snapshot_stop(v_bus_snap_t *snap);
#endif

// --- Statistics (B7) ----------------------------------------------------------
// Explicit getters, never internals. A snapshot is per-counter best-effort: the
// bus promises each value is a real one it held, not that the set is coherent
// (§9 — a consistent system snapshot is an explicit non-goal).
typedef struct {
  uint32_t published; // messages this topic made visible
  uint32_t dropped;   // publishes the full pool refused
  uint32_t evicted;   // messages overwritten before every reader took them
  uint16_t queued;    // messages waiting now
  uint16_t blocks;    // pool blocks those messages hold
  uint16_t subs;      // subscribers
  uint16_t reserved;  // blocks stashed for this topic (cfg.reserve)
  uint16_t lent;      // of those, currently lent to overwrite topics
  uint16_t borrowed;  // blocks this topic owes other topics
} v_bus_topic_stats_t;

typedef struct {
  uint16_t block_size;
  uint16_t block_count;
  uint16_t free_blocks; // in the pool right now
  uint16_t topics;      // declared on this bus
} v_bus_stats_t;

// VA_PASS, or V_BUS_EINVAL on a bad argument.
int v_bus_topic_stats(const v_bus_topic_t *topic, v_bus_topic_stats_t *out);
int v_bus_stats(const v_bus_t *bus, v_bus_stats_t *out);

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
