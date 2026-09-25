// Bus IPC subsystem — see include/bus.h and docs/plans/bus-subsystem.md.
//
// B1: the block pool. Free blocks form a stack threaded through desc[].next,
// so taking or returning a block is O(1) whatever the pool size. Every
// mutation runs under ENTER_CRITICAL_FROM_ISR: short, bounded, and usable from
// the single-producer ISR publish path (plan §6.3, §7).
//
// B2: topics. A message is a chain of blocks whose first block starts with a
// msg_hdr_t; a topic's messages are linked oldest -> newest through hdr.link.
// Each subscription holds a cursor to its next unread message. Payload copies
// run outside the critical section: a message isn't visible until it is
// linked, and pop reclaims it only after the last reader is done.
//
// B3: unsubscribe releases what the subscriber owed; V_BUS_OVERWRITE evicts a
// topic's oldest message, moving any cursor on it to the next one at once.
// The one thing that can't be fixed up eagerly is a pop already copying the
// evicted message: it checks the head block's epoch (bumped on every free)
// after the copy and, if it changed, discards the copy and reads again.
//
// Soft reservations: a topic with cfg.reserve keeps that many blocks in its
// own stash (a free stack like the pool's). Blocks are interchangeable, so a
// loan is only a count: an overwrite topic that borrowed repays with whatever
// block it frees next. Every free goes through free_chain, which repays loans
// first, refills the owner's stash (stash + lent <= reserve), and only then
// returns blocks to the shared pool.
//
// Pipes (cfg.pipe): the reservation becomes a circular chain of slots owned by
// the topic. The producer alone writes next_seq and pipe_wr, the one consumer
// alone writes its expect/cursor; a message is published by stamping its slot
// (hdr.seq = 2*seq) and then bumping next_seq, with barriers in between — the
// SPSC protocol, no critical section. In overwrite mode the producer marks a
// slot odd (being written) first, and a reader treats any slot whose stamp
// isn't the one it expects as lost (a seqlock), re-checking after its copy.
//
// Zero copy: v_bus_reserve/commit hand out and publish a single pool block;
// v_bus_peek/release read one in place. A peeked message is pinned, which is
// what keeps the in-place view valid: eviction stops at a pinned oldest
// message instead. (pop keeps the epoch retry so a copying reader never holds
// the writer up; a peek holder does, briefly, which is the price of no copy.)
#include "bus.h"
#include "port.h"  // ENTER/EXIT_CRITICAL_FROM_ISR
#include "utils.h" // v_memcpy

// Header at the start of a message's first block.
typedef struct {
  uint32_t seq;  // assigned at link time: monotonic in queue order
  uint16_t link; // next (newer) message in the topic, or V_BUS_NIL
  uint16_t len;  // payload bytes
  uint16_t refs; // subscribers that still have to read it
  uint16_t pins; // v_bus_peek views open on it: overwrite won't evict it
} msg_hdr_t;
_Static_assert(sizeof(msg_hdr_t) == V_BUS_HDR_SIZE, "V_BUS_HDR_SIZE drift");

// desc.used values: 0 free (pool), 1 in use, RESERVED a v_bus_reserve block
// not yet committed (so a stale or repeated ticket can't re-link a queued
// message), STASHED idle in a topic's reservation.
#define RESERVED 2u
#define STASHED 3u
#define PIPE_SLOT 4u

// Lock-free pipe fields are shared between exactly one producer and one
// consumer context; these keep the compiler from caching or tearing them.
#if VAIOS_DEVFS
static void bus_register(v_bus_t *bus); // fd layer: resolve a topic by name
#endif

#define LOAD32(x) (*(volatile const uint32_t *)&(x))
#define STORE32(x, v) (*(volatile uint32_t *)&(x) = (v))

int v_bus_init(v_bus_t *bus, void *blocks, v_bus_desc_t *desc,
               uint16_t block_size, uint16_t block_count) {
  if (!bus || !blocks || !desc || block_size < V_BUS_HDR_SIZE ||
      block_size % 4u || !block_count || block_count >= V_BUS_NIL)
    return VA_FAIL; // %4: the header in each first block stays word-aligned
  for (uint16_t i = 0; i < block_count; i++) {
    desc[i].next = (uint16_t)(i + 1u < block_count ? i + 1u : V_BUS_NIL);
    desc[i].epoch = 0;
    desc[i].used = 0;
  }
  *bus = (v_bus_t){.topics = 0,
                   .blocks = (uint8_t *)blocks,
                   .desc = desc,
                   .block_size = block_size,
                   .block_count = block_count,
                   .free_head = 0,
                   .free_count = block_count};
#if VAIOS_DEVFS
  bus_register(bus); // so v_bus_open can find this bus's topics by name
#endif
  return VA_PASS;
}

uint16_t v_bus_free_blocks(const v_bus_t *bus) { return bus->free_count; }

static int topics_ok(const v_bus_t *bus);

int v_bus_check(const v_bus_t *bus) {
  uint16_t on_list = 0;
  for (uint16_t i = bus->free_head; i != V_BUS_NIL; i = bus->desc[i].next) {
    if (i >= bus->block_count || bus->desc[i].used ||
        on_list == bus->block_count) // out of range, live, or a cycle
      return VA_FAIL;
    on_list++;
  }
  uint16_t used = 0;
  for (uint16_t i = 0; i < bus->block_count; i++)
    used = (uint16_t)(used + (bus->desc[i].used != 0));
  return on_list == bus->free_count &&
                 used == bus->block_count - bus->free_count &&
                 topics_ok(bus)
             ? VA_PASS
             : VA_FAIL;
}

// --- Topics and messages (B2) ------------------------------------------------
// Block address for internal callers, whose indices come from the allocator,
// a cursor or a queue link (all in range by construction).
static uint8_t *blk(const v_bus_t *bus, uint16_t idx) {
  return bus->blocks + (uint32_t)idx * bus->block_size;
}
static msg_hdr_t *hdr(const v_bus_t *bus, uint16_t msg) {
  return (msg_hdr_t *)(void *)blk(bus, msg);
}

static int name_eq(const char *a, const char *b) {
  while (*a && *a == *b) {
    a++;
    b++;
  }
  return *a == *b;
}

// Blocks a message of `len` payload bytes needs: the first block also holds
// the header.
static uint32_t blocks_for(const v_bus_t *bus, uint32_t len) {
  uint32_t first = bus->block_size - V_BUS_HDR_SIZE;
  return len <= first ? 1u : 1u + (len - first + bus->block_size - 1u) /
                                      bus->block_size;
}

// Copy between a flat buffer and a message's payload, which starts after the
// header in the first block and continues through the chain. to_msg selects
// the direction.
static void copy_payload(v_bus_t *bus, uint16_t msg, void *flat, uint16_t len,
                         int to_msg) {
  uint8_t *p = (uint8_t *)flat;
  uint16_t off = V_BUS_HDR_SIZE;
  for (uint16_t b = msg; len; b = bus->desc[b].next, off = 0) {
    uint16_t n = (uint16_t)(bus->block_size - off);
    if (n > len)
      n = len;
    uint8_t *d = blk(bus, b) + off;
    if (to_msg)
      v_memcpy(d, p, n);
    else
      v_memcpy(p, d, n);
    p += n;
    len = (uint16_t)(len - n);
  }
}

int v_bus_topic_declare(v_bus_t *bus, v_bus_topic_t *topic, const char *name,
                        const v_bus_topic_cfg_t *cfg) {
  if (!bus || !topic || !name)
    return V_BUS_EINVAL;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  for (v_bus_topic_t *t = bus->topics; t; t = t->next) {
    if (t == topic || name_eq(t->name, name)) {
      EXIT_CRITICAL_FROM_ISR(s);
      return V_BUS_EINVAL;
    }
  }
  uint16_t reserve = cfg ? cfg->reserve : 0;
  int pipe = cfg && cfg->pipe;
  if (pipe && !reserve) {
    EXIT_CRITICAL_FROM_ISR(s);
    return V_BUS_EINVAL; // a pipe is its reservation: it needs slots
  }
  if (reserve > bus->free_count) {
    EXIT_CRITICAL_FROM_ISR(s);
    return VA_FAIL; // the pool can't cover the reservation right now
  }
  *topic = (v_bus_topic_t){.bus = bus,
                           .name = name,
                           .next = bus->topics,
                           .subs = 0,
                           .cfg = cfg ? *cfg : (v_bus_topic_cfg_t){0},
                           .head = V_BUS_NIL,
                           .tail = V_BUS_NIL,
                           .stash = V_BUS_NIL};
  if (pipe) { // pool -> a closed ring of slots, stamped "never published"
    uint16_t first = V_BUS_NIL, prev = V_BUS_NIL;
    for (uint16_t k = 0; k < reserve; k++) {
      uint16_t b = bus->free_head;
      bus->free_head = bus->desc[b].next;
      bus->desc[b].used = PIPE_SLOT;
      hdr(bus, b)->seq = 1u; // odd: never matches a reader's 2*seq
      if (prev == V_BUS_NIL)
        first = b;
      else
        bus->desc[prev].next = b;
      prev = b;
    }
    bus->desc[prev].next = first;
    topic->pipe_wr = first;
    topic->blocks = reserve;
  } else {
    for (uint16_t k = 0; k < reserve; k++) { // pool -> stash
      uint16_t b = bus->free_head;
      bus->free_head = bus->desc[b].next;
      bus->desc[b].next = topic->stash;
      bus->desc[b].used = STASHED;
      topic->stash = b;
    }
    topic->stash_count = reserve;
  }
  bus->free_count = (uint16_t)(bus->free_count - reserve);
  bus->topics = topic;
  EXIT_CRITICAL_FROM_ISR(s);
  return VA_PASS;
}

int v_bus_subscribe(v_bus_topic_t *topic, v_bus_sub_t *sub) {
  if (!topic || !topic->bus || !sub)
    return V_BUS_EINVAL;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  for (v_bus_sub_t *x = topic->subs; x; x = x->next) {
    if (x == sub) { // already attached: re-linking would loop the list
      EXIT_CRITICAL_FROM_ISR(s);
      return V_BUS_EINVAL;
    }
  }
  if (topic->cfg.pipe && topic->nsubs) {
    EXIT_CRITICAL_FROM_ISR(s);
    return V_BUS_EBUSY; // a pipe has exactly one subscriber
  }
  *sub = (v_bus_sub_t){.topic = topic,
                       .next = topic->subs,
                       .cursor = topic->cfg.pipe ? topic->pipe_wr : V_BUS_NIL,
                       .peeked = V_BUS_NIL,
                       .expect = topic->next_seq};
  topic->subs = sub;
  topic->nsubs++;
  EXIT_CRITICAL_FROM_ISR(s);
  return VA_PASS;
}

// --- Block routing: stash, pool, loans ----------------------------------------
// All below run with the critical section held.

static v_bus_topic_t *lender_with_idle(v_bus_t *bus, const v_bus_topic_t *me) {
  for (v_bus_topic_t *t = bus->topics; t; t = t->next)
    if (t != me && t->stash_count)
      return t;
  return 0;
}

static uint16_t stash_pop(v_bus_topic_t *t) {
  uint16_t b = t->stash;
  t->stash = t->bus->desc[b].next;
  t->stash_count--;
  return b;
}

static void stash_push(v_bus_topic_t *t, uint16_t b) {
  t->bus->desc[b].used = STASHED;
  t->bus->desc[b].next = t->stash;
  t->stash = b;
  t->stash_count++;
}

// One block for `t`: its stash, then the pool, then (overwrite topics only)
// another topic's idle stash block on loan. V_BUS_NIL if none of those.
static uint16_t take_one(v_bus_topic_t *t) {
  v_bus_t *bus = t->bus;
  uint16_t b;
  v_bus_topic_t *lender;
  if (t->stash_count) {
    b = stash_pop(t);
  } else if (bus->free_count) {
    b = bus->free_head;
    bus->free_head = bus->desc[b].next;
    bus->free_count--;
  } else if (t->cfg.overflow == V_BUS_OVERWRITE &&
             (lender = lender_with_idle(bus, t))) {
    b = stash_pop(lender);
    lender->lent++;
    t->borrowed++;
  } else {
    return V_BUS_NIL;
  }
  bus->desc[b].used = 1;
  bus->desc[b].next = V_BUS_NIL;
  return b;
}

// A block `t` no longer needs: repay a loan, else refill t's stash, else
// back to the pool. The epoch bump lets a copying reader spot the reuse.
static void give_one(v_bus_topic_t *t, uint16_t b) {
  v_bus_t *bus = t->bus;
  bus->desc[b].epoch++;
  if (t->borrowed) {
    v_bus_topic_t *l = bus->topics;
    while (!l->lent) // sum(lent) == sum(borrowed): one exists
      l = l->next;
    stash_push(l, b);
    l->lent--;
    t->borrowed--;
  } else if (t->stash_count + t->lent < t->cfg.reserve) {
    stash_push(t, b);
  } else {
    bus->desc[b].used = 0;
    bus->desc[b].next = bus->free_head;
    bus->free_head = b;
    bus->free_count++;
  }
}

// Free a whole chain on behalf of `t` (it ends in V_BUS_NIL). Task or ISR.
static void free_chain(v_bus_topic_t *t, uint16_t head) {
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  while (head != V_BUS_NIL) {
    uint16_t next = t->bus->desc[head].next;
    give_one(t, head);
    head = next;
  }
  EXIT_CRITICAL_FROM_ISR(s);
}

// Caller holds the critical section. Unlink the topic's oldest message and
// return it for freeing; its blocks leave the topic's count.
static inline uint16_t take_head(v_bus_topic_t *topic) {
  v_bus_t *bus = topic->bus;
  uint16_t msg = topic->head;
  msg_hdr_t *h = hdr(bus, msg);
  topic->head = h->link;
  if (topic->tail == msg)
    topic->tail = V_BUS_NIL;
  topic->blocks = (uint16_t)(topic->blocks - blocks_for(bus, h->len));
  return msg;
}

int v_bus_unsubscribe(v_bus_sub_t *sub) {
  if (!sub || !sub->topic)
    return V_BUS_EINVAL;
  v_bus_topic_t *topic = sub->topic;
  v_bus_t *bus = topic->bus;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  v_bus_sub_t **pp = &topic->subs;
  while (*pp && *pp != sub)
    pp = &(*pp)->next;
  if (!*pp) {
    EXIT_CRITICAL_FROM_ISR(s);
    return V_BUS_EINVAL;
  }
  *pp = sub->next;
  topic->nsubs--;
  if (topic->cfg.pipe) { // a pipe reader holds no claims or pins
    EXIT_CRITICAL_FROM_ISR(s);
    sub->topic = 0;
    return VA_PASS;
  }
  if (sub->peeked != V_BUS_NIL) // left holding a peek: drop its pin
    hdr(bus, sub->peeked)->pins--;
  // Drop its claim on every message it hadn't read...
  for (uint16_t m = sub->cursor; m != V_BUS_NIL; m = hdr(bus, m)->link)
    hdr(bus, m)->refs--;
  // ...and free those nobody else is waiting for. Every reader goes in order,
  // so a message's refs never exceed a newer one's: the zeros are a run at
  // the head. ponytail: freed inside the critical section, O(queued blocks);
  // unsubscribe is rare and task-level. Hand them out after it if that bites.
  while (topic->head != V_BUS_NIL && !hdr(bus, topic->head)->refs)
    free_chain(topic, take_head(topic));
  EXIT_CRITICAL_FROM_ISR(s);
  sub->topic = 0;
  return VA_PASS;
}

// V_BUS_OVERWRITE: evict the oldest message; readers that hadn't read it move
// on to the next one (they see the gap as `missed` at their next pop). A
// pinned (peeked) oldest message stops eviction: its reader is looking at it.
static int evict_oldest(v_bus_topic_t *topic) {
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  if (topic->head == V_BUS_NIL || hdr(topic->bus, topic->head)->pins) {
    EXIT_CRITICAL_FROM_ISR(s);
    return 0;
  }
  uint16_t link = hdr(topic->bus, topic->head)->link;
  for (v_bus_sub_t *x = topic->subs; x; x = x->next)
    if (x->cursor == topic->head)
      x->cursor = link;
  free_chain(topic, take_head(topic));
  EXIT_CRITICAL_FROM_ISR(s);
  return 1;
}

// A topic short of its own reservation takes a lent block back: evict the
// oldest message of some borrower (an overwrite topic, so losing its oldest is
// its policy anyway). Freed blocks repay lenders. 0 if no borrower can give.
static int reclaim_one(v_bus_topic_t *t) {
  for (v_bus_topic_t *b = t->bus->topics; b; b = b->next)
    if (b->borrowed && evict_oldest(b))
      return 1;
  return 0;
}

// Take n blocks for a new message on `topic`, all or nothing: reservation,
// pool, loans (overwrite topics), then taking back its own lent blocks, then
// (overwrite) evicting its own oldest messages. Only starts when it can end
// well — never evicts for a message that still wouldn't fit.
// ponytail: runs under one critical section, bounded by n + evictions.
static inline uint16_t alloc_msg(v_bus_topic_t *topic, uint32_t n) {
  v_bus_t *bus = topic->bus;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  if (topic->stash_count >= n) { // hot path: all from the reservation
    uint16_t head = V_BUS_NIL;
    for (uint32_t k = 0; k < n; k++) {
      uint16_t b = stash_pop(topic);
      bus->desc[b].used = 1;
      bus->desc[b].next = head;
      head = b;
    }
    EXIT_CRITICAL_FROM_ISR(s);
    return head;
  }
  uint32_t can = topic->stash_count + bus->free_count + topic->lent;
  if (topic->cfg.overflow == V_BUS_OVERWRITE) {
    can += topic->blocks; // its own queue is fair game
    for (v_bus_topic_t *t = bus->topics; t; t = t->next)
      if (t != topic)
        can += t->stash_count; // borrowable
  }
  if (can < n) {
    EXIT_CRITICAL_FROM_ISR(s);
    return V_BUS_NIL;
  }
  uint16_t head = V_BUS_NIL;
  for (uint32_t k = 0; k < n; k++) {
    uint16_t b = take_one(topic);
    while (b == V_BUS_NIL &&
           ((topic->lent && reclaim_one(topic)) ||
            (topic->cfg.overflow == V_BUS_OVERWRITE && evict_oldest(topic))))
      b = take_one(topic);
    if (b == V_BUS_NIL) { // a pinned message blocked the way: give back
      free_chain(topic, head);
      head = V_BUS_NIL;
      break;
    }
    bus->desc[b].next = head;
    head = b;
  }
  EXIT_CRITICAL_FROM_ISR(s);
  return head;
}

// Blocking mode (B6): wake the readers that asked to sleep. Called with the
// topic's critical section held, from task OR ISR context — the from_isr form is
// the one that is safe in both (it reports the wake instead of yielding), and
// the caller pends the switch once afterwards, outside the section.
static inline void notify_sub(const v_bus_sub_t *x, int *woke) {
  if (x->notify)
    v_semaphore_give_from_isr(x->notify, woke);
}

// Make a filled-in message visible. seq and refs are set under the same
// critical section as subscribe, so refs counts exactly the subscribers whose
// cursor can reach it. Nobody subscribed any more: free it instead.
static inline void link_msg(v_bus_topic_t *topic, uint16_t msg, uint16_t len,
                     uint32_t n) {
  v_bus_t *bus = topic->bus;
  msg_hdr_t *h = hdr(bus, msg);
  h->link = V_BUS_NIL;
  h->len = len;
  h->pins = 0;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  if (!topic->nsubs) {
    EXIT_CRITICAL_FROM_ISR(s);
    free_chain(topic, msg);
    return;
  }
  h->seq = topic->next_seq++;
  h->refs = topic->nsubs;
  if (topic->tail != V_BUS_NIL)
    hdr(bus, topic->tail)->link = msg;
  else
    topic->head = msg;
  topic->tail = msg;
  topic->blocks = (uint16_t)(topic->blocks + n);
  int woke = 0;
  for (v_bus_sub_t *x = topic->subs; x; x = x->next) {
    if (x->cursor == V_BUS_NIL) // caught up: this is its next message
      x->cursor = msg;
    notify_sub(x, &woke);
  }
  EXIT_CRITICAL_FROM_ISR(s);
  if (woke) // a sleeping reader outranks us: switch now, not at the next tick
    v_port_trigger_pendsv();
}

#ifdef VAIOS_HOST_TEST
// Host-test seam: runs between a copying read and its commit, so a test can
// overwrite/evict the message mid-read (on target: an ISR publish preempting).
void (*v_bus_test_mid_pop)(void);
#endif

// --- Pipe paths (lock-free, one producer + one consumer) -----------------------
// Producer: the slot the next message goes into, or V_BUS_NIL when a drop
// pipe is full. An overwrite pipe marks it "being written" first, so a reader
// that is behind by a whole ring can't take its old contents for current.
static inline uint16_t pipe_claim(v_bus_topic_t *t) {
  uint16_t b = t->pipe_wr;
  uint32_t w = t->next_seq;
  if (t->cfg.overflow != V_BUS_OVERWRITE) {
    v_bus_sub_t *sub = t->subs;
    if (sub && w - LOAD32(sub->expect) >= t->cfg.reserve)
      return V_BUS_NIL; // full: the reader still owns every slot
  } else {
    STORE32(hdr(t->bus, b)->seq, 2u * w + 1u);
    V_PORT_MB();
  }
  return b;
}

// Producer: publish the claimed slot (payload already written).
static inline void pipe_publish(v_bus_topic_t *t, uint16_t b, uint16_t len) {
  uint32_t w = t->next_seq;
  hdr(t->bus, b)->len = len;
  V_PORT_MB(); // payload + len before the stamp
  STORE32(hdr(t->bus, b)->seq, 2u * w);
  V_PORT_MB(); // stamp before the reader can see next_seq move
  STORE32(t->next_seq, w + 1u);
  t->pipe_wr = t->bus->desc[b].next;
  if (t->subs && t->subs->notify) { // the pipe's one reader, if it sleeps
    int woke = 0;
    notify_sub(t->subs, &woke);
    if (woke)
      v_port_trigger_pendsv();
  }
}

// Consumer: find the oldest message it hasn't read, copying it into `out` or
// (view != NULL) pointing at it. Slots it finds overwritten are counted as
// missed. Nothing is committed here on failure, so a retry starts over.
static int pipe_read(v_bus_sub_t *sub, void *out, uint16_t cap,
                     uint16_t *out_len, uint32_t *missed, const void **view) {
  v_bus_topic_t *t = sub->topic;
  v_bus_t *bus = t->bus;
  uint32_t e0 = sub->expect, r = e0, depth = t->cfg.reserve;
  uint16_t rd = sub->cursor;
  for (;;) {
    uint32_t w = LOAD32(t->next_seq);
    V_PORT_MB();
    if (w == r)
      return VA_FAIL; // caught up
    if (w - r > depth) { // lapped: jump to the oldest slot still holding data
      for (uint32_t k = (w - depth - r) % depth; k; k--)
        rd = bus->desc[rd].next;
      r = w - depth;
    }
    msg_hdr_t *h = hdr(bus, rd);
    if (LOAD32(h->seq) != 2u * r) { // overwritten / being written: lost
      r++;
      rd = bus->desc[rd].next;
      continue;
    }
    uint16_t len = h->len;
    *out_len = len;
    if (view) { // peek: pinned by protocol on a drop pipe, checked on release
      *view = blk(bus, rd) + V_BUS_HDR_SIZE;
      sub->expect = r;
      sub->cursor = rd;
      sub->peeked = rd;
    } else {
      if (len > cap)
        return V_BUS_EMSGSIZE;
      copy_payload(bus, rd, out, len, 0);
#ifdef VAIOS_HOST_TEST
      if (v_bus_test_mid_pop)
        v_bus_test_mid_pop();
#endif
      V_PORT_MB();
      if (LOAD32(h->seq) != 2u * r) { // overwritten under the copy
        r++;
        rd = bus->desc[rd].next;
        continue;
      }
      sub->expect = r + 1u;
      sub->cursor = bus->desc[rd].next;
    }
    if (missed)
      *missed = r - e0;
    return VA_PASS;
  }
}

// Consumer fast path: the next slot is published and intact (not lapped, stamp
// matches) — the common case. Returns its header, or NULL to take pipe_read.
static inline msg_hdr_t *pipe_next(const v_bus_sub_t *sub) {
  const v_bus_topic_t *t = sub->topic;
  uint32_t r = sub->expect, w = LOAD32(t->next_seq);
  V_PORT_MB();
  if (w == r || w - r > t->cfg.reserve)
    return 0;
  msg_hdr_t *h = hdr(t->bus, sub->cursor);
  return LOAD32(h->seq) == 2u * r ? h : 0;
}

int v_bus_publish(v_bus_topic_t *topic, const void *payload, uint16_t len) {
  if (!topic || !topic->bus || (len && !payload))
    return V_BUS_EINVAL;
  if (topic->cfg.pipe) {
    if (len > topic->bus->block_size - V_BUS_HDR_SIZE)
      return V_BUS_ESPLIT; // a pipe slot is one block
    if (!topic->nsubs)
      return VA_PASS; // nobody to deliver to
    uint16_t b = pipe_claim(topic);
    if (b == V_BUS_NIL)
      return VA_FAIL; // full (drop)
    copy_payload(topic->bus, b, (void *)payload, len, 1);
    pipe_publish(topic, b, len);
    return VA_PASS;
  }
  v_bus_t *bus = topic->bus;
  uint32_t n = blocks_for(bus, len);
  if (n > bus->block_count)
    return V_BUS_EINVAL; // could never fit
  if (!topic->nsubs)
    return VA_PASS; // nobody to deliver to: keep nothing
  uint16_t msg = alloc_msg(topic, n);
  if (msg == V_BUS_NIL)
    return VA_FAIL; // dropped
  copy_payload(bus, msg, (void *)payload, len, 1); // invisible until linked
  link_msg(topic, msg, len, n);
  return VA_PASS;
}


static int ticket_ok(const v_bus_topic_t *topic, uint16_t ticket) {
  return topic && topic->bus && ticket < topic->bus->block_count &&
         topic->bus->desc[ticket].used == RESERVED;
}

void *v_bus_reserve(v_bus_topic_t *topic, uint16_t max_len, uint16_t *ticket) {
  if (!topic || !topic->bus || !ticket ||
      max_len > topic->bus->block_size - V_BUS_HDR_SIZE)
    return 0;
  if (topic->cfg.pipe) { // hot path first
    uint16_t b = pipe_claim(topic);
    if (b == V_BUS_NIL)
      return 0;
    *ticket = b;
    return blk(topic->bus, b) + V_BUS_HDR_SIZE;
  }
  uint16_t msg = alloc_msg(topic, 1);
  if (msg == V_BUS_NIL)
    return 0;
  topic->bus->desc[msg].used = RESERVED;
  *ticket = msg;
  return blk(topic->bus, msg) + V_BUS_HDR_SIZE;
}

int v_bus_commit(v_bus_topic_t *topic, uint16_t ticket, uint16_t len) {
  if (topic && topic->cfg.pipe) {
    if (ticket != topic->pipe_wr ||
        len > topic->bus->block_size - V_BUS_HDR_SIZE)
      return V_BUS_EINVAL;
    if (topic->nsubs) // nobody subscribed: the claim is simply dropped
      pipe_publish(topic, ticket, len);
    return VA_PASS;
  }
  if (!ticket_ok(topic, ticket) ||
      len > topic->bus->block_size - V_BUS_HDR_SIZE)
    return V_BUS_EINVAL;
  topic->bus->desc[ticket].used = 1;
  link_msg(topic, ticket, len, 1);
  return VA_PASS;
}

int v_bus_cancel(v_bus_topic_t *topic, uint16_t ticket) {
  if (topic && topic->cfg.pipe) // nothing was published; on an overwrite pipe
    return ticket == topic->pipe_wr ? VA_PASS : V_BUS_EINVAL; // the oldest
                                                    // message is gone, though
  if (!ticket_ok(topic, ticket))
    return V_BUS_EINVAL;
  free_chain(topic, ticket);
  return VA_PASS;
}

// Caller holds the critical section. `sub` is done with `msg` (its cursor):
// move on, drop its reference, and return msg if that freed it (the caller
// frees the blocks after leaving the critical section).
static inline uint16_t consume(v_bus_sub_t *sub, uint16_t msg) {
  v_bus_topic_t *topic = sub->topic;
  msg_hdr_t *h = hdr(topic->bus, msg);
  sub->expect = h->seq + 1u;
  sub->cursor = h->link; // V_BUS_NIL if we're caught up
  if (--h->refs)
    return V_BUS_NIL;
  // Every reader reads in order, so the last reference always drops on the
  // oldest message.
  take_head(topic);
  return msg;
}

int v_bus_pop(v_bus_sub_t *sub, void *out, uint16_t cap, uint16_t *out_len,
              uint32_t *missed) {
  if (!sub || !sub->topic || !out_len || (cap && !out))
    return V_BUS_EINVAL;
  if (sub->peeked != V_BUS_NIL)
    return V_BUS_EBUSY; // release the peek first
  if (sub->topic->cfg.pipe) {
    msg_hdr_t *h = pipe_next(sub);
    if (h && h->len <= cap) { // copy, then make sure it wasn't overwritten
      uint32_t r = sub->expect;
      uint16_t len = h->len;
      copy_payload(sub->topic->bus, sub->cursor, out, len, 0);
#ifdef VAIOS_HOST_TEST
      if (v_bus_test_mid_pop)
        v_bus_test_mid_pop();
#endif
      V_PORT_MB();
      if (LOAD32(h->seq) == 2u * r) {
        *out_len = len;
        if (missed)
          *missed = 0;
        sub->expect = r + 1u;
        sub->cursor = sub->topic->bus->desc[sub->cursor].next;
        return VA_PASS;
      }
    }
    return pipe_read(sub, out, cap, out_len, missed, 0);
  }
  v_bus_t *bus = sub->topic->bus;

  for (;;) {
    uint32_t s = ENTER_CRITICAL_FROM_ISR();
    uint16_t msg = sub->cursor;
    if (msg == V_BUS_NIL) {
      EXIT_CRITICAL_FROM_ISR(s);
      return VA_FAIL;
    }
    uint16_t epoch = bus->desc[msg].epoch;
    uint16_t len = hdr(bus, msg)->len;
    EXIT_CRITICAL_FROM_ISR(s);
    *out_len = len;
    if (len > cap)
      return V_BUS_EMSGSIZE; // stays unread

    copy_payload(bus, msg, out, len, 0);
#ifdef VAIOS_HOST_TEST
    if (v_bus_test_mid_pop)
      v_bus_test_mid_pop();
#endif

    s = ENTER_CRITICAL_FROM_ISR();
    if (bus->desc[msg].epoch != epoch) {
      // Evicted while we copied: `out` may be torn. The eviction already moved
      // our cursor on; read that one instead.
      EXIT_CRITICAL_FROM_ISR(s);
      continue;
    }
    if (missed)
      *missed = hdr(bus, msg)->seq - sub->expect; // overwritten before we read
    uint16_t freed = consume(sub, msg);
    EXIT_CRITICAL_FROM_ISR(s);
    if (freed != V_BUS_NIL)
      free_chain(sub->topic, freed);
    return VA_PASS;
  }
}

int v_bus_peek(v_bus_sub_t *sub, const void **data, uint16_t *len,
               uint32_t *missed) {
  if (!sub || !sub->topic || !data || !len)
    return V_BUS_EINVAL;
  if (sub->topic->cfg.pipe) {
    if (sub->peeked != V_BUS_NIL)
      return V_BUS_EBUSY;
    msg_hdr_t *h = pipe_next(sub);
    if (!h)
      return pipe_read(sub, 0, 0, len, missed, data);
    *len = h->len;
    *data = (const uint8_t *)h + V_BUS_HDR_SIZE;
    if (missed)
      *missed = 0;
    sub->peeked = sub->cursor;
    return VA_PASS;
  }
  v_bus_t *bus = sub->topic->bus;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  uint16_t msg = sub->cursor;
  int r = VA_PASS;
  if (sub->peeked != V_BUS_NIL)
    r = V_BUS_EBUSY;
  else if (msg == V_BUS_NIL)
    r = VA_FAIL;
  else if (blocks_for(bus, hdr(bus, msg)->len) > 1)
    r = V_BUS_ESPLIT; // not contiguous: read it with v_bus_pop
  if (r == VA_PASS) {
    msg_hdr_t *h = hdr(bus, msg);
    h->pins++; // from here eviction leaves it alone
    sub->peeked = msg;
    *len = h->len;
    if (missed)
      *missed = h->seq - sub->expect;
    *data = blk(bus, msg) + V_BUS_HDR_SIZE;
  }
  EXIT_CRITICAL_FROM_ISR(s);
  return r;
}

int v_bus_release(v_bus_sub_t *sub) {
  if (!sub || !sub->topic || sub->peeked == V_BUS_NIL)
    return V_BUS_EINVAL;
  v_bus_t *bus = sub->topic->bus;
  if (sub->topic->cfg.pipe) {
    uint16_t rd = sub->peeked;
    uint32_t r = sub->expect;
    V_PORT_MB();
    int stale = LOAD32(hdr(bus, rd)->seq) != 2u * r; // overwrite pipe only
    sub->peeked = V_BUS_NIL;
    sub->expect = r + 1u;
    sub->cursor = bus->desc[rd].next;
    return stale ? V_BUS_ESTALE : VA_PASS;
  }
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  uint16_t msg = sub->peeked;
  hdr(bus, msg)->pins--;
  sub->peeked = V_BUS_NIL;
  uint16_t freed = consume(sub, msg); // pinned, so still our cursor
  EXIT_CRITICAL_FROM_ISR(s);
  if (freed != V_BUS_NIL)
    free_chain(sub->topic, freed);
  return VA_PASS;
}

// v_bus_check, per topic: head..tail is a well-formed chain of live messages,
// each still owed to at least one subscriber, their blocks add up to the
// topic's count, and every cursor points into the chain.
static int topics_ok(const v_bus_t *bus) {
  const v_bus_t *b = bus;
  uint32_t lent = 0, borrowed = 0, accounted = bus->free_count;
  for (uint16_t i = 0; i < bus->block_count; i++)
    accounted += bus->desc[i].used == RESERVED; // open v_bus_reserve tickets
  for (const v_bus_topic_t *t = bus->topics; t; t = t->next) {
    if (t->cfg.pipe) { // a closed ring of exactly `reserve` pipe slots
      uint16_t i = t->pipe_wr, n = 0;
      do {
        if (i >= bus->block_count || bus->desc[i].used != PIPE_SLOT ||
            ++n > t->cfg.reserve)
          return 0;
        i = bus->desc[i].next;
      } while (i != t->pipe_wr);
      if (n != t->cfg.reserve || t->blocks != n || t->stash_count ||
          t->lent || t->borrowed || t->nsubs > 1 || t->head != V_BUS_NIL)
        return 0;
      if (t->subs) { // its reader is never ahead, and sits exactly
        const v_bus_sub_t *x = t->subs; // (next_seq - expect) slots behind
        uint32_t behind = t->next_seq - x->expect;
        if ((int32_t)behind < 0 ||
            (t->cfg.overflow != V_BUS_OVERWRITE && behind > n))
          return 0;
        uint16_t c = t->pipe_wr, m = 0; // forward distance write -> read
        while (c != x->cursor && ++m < n)
          c = bus->desc[c].next;
        if (c != x->cursor || m != (n - behind % n) % n)
          return 0;
      }
      accounted += n;
      continue;
    }
    uint16_t k = 0;
    for (uint16_t i = t->stash; i != V_BUS_NIL; i = bus->desc[i].next)
      if (i >= bus->block_count || bus->desc[i].used != STASHED ||
          ++k > t->stash_count)
        return 0;
    if (k != t->stash_count || t->stash_count + t->lent > t->cfg.reserve)
      return 0;
    lent += t->lent;
    borrowed += t->borrowed;
    accounted += t->stash_count + t->blocks;
    if ((t->head == V_BUS_NIL) != (t->tail == V_BUS_NIL))
      return 0;
    uint16_t last = V_BUS_NIL, n = 0;
    uint32_t blocks = 0;
    for (uint16_t m = t->head; m != V_BUS_NIL; m = hdr(b, m)->link) {
      if (m >= bus->block_count || !bus->desc[m].used || !hdr(b, m)->refs ||
          ++n > bus->block_count)
        return 0;
      uint32_t want = blocks_for(bus, hdr(b, m)->len), got = 0;
      for (uint16_t c = m; c != V_BUS_NIL; c = bus->desc[c].next)
        if (c >= bus->block_count || bus->desc[c].used != 1 || ++got > want)
          return 0; // every block of a queued message is in use, none extra
      if (got != want)
        return 0;
      blocks += want;
      last = m;
    }
    if (last != t->tail || blocks != t->blocks)
      return 0;
    uint16_t nsubs = 0;
    uint32_t peeks = 0;
    for (const v_bus_sub_t *x = t->subs; x; x = x->next) {
      uint16_t m = t->head;
      while (m != V_BUS_NIL && m != x->cursor)
        m = hdr(b, m)->link;
      if (m != x->cursor || ++nsubs > t->nsubs) // off the chain, or list loops
        return 0;
      if (x->peeked != V_BUS_NIL) { // a peek sits on its cursor, pinned
        if (x->peeked != x->cursor || !hdr(b, x->peeked)->pins)
          return 0;
        peeks++;
      }
    }
    uint32_t pins = 0;
    for (uint16_t m = t->head; m != V_BUS_NIL; m = hdr(b, m)->link)
      pins += hdr(b, m)->pins;
    if (nsubs != t->nsubs || pins != peeks)
      return 0;
  }
  // Loans balance, and every block is somewhere: nothing leaked.
  return lent == borrowed && accounted == bus->block_count;
}

// --- User access: topics on the fd table (VAIOS_DEVFS) ------------------------
// A task can't touch the bus itself (kernel memory, BASEPRI critical sections),
// so it opens a topic by name and goes through syscalls. Each open holds a
// kernel-side subscription; the fd's close drops it, which is also what keeps a
// task that exits mid-stream from pinning queued messages (H6).
#if VAIOS_DEVFS
#include "syscall.h"
#include "vfile.h"

// Buses register here so a name can be resolved without the caller naming the
// bus. Bounded and dedupe-on-init: a re-initialised bus does not queue up.
#define BUS_REG_MAX 8
static v_bus_t *bus_reg[BUS_REG_MAX];

static void bus_register(v_bus_t *bus) {
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  int free_slot = -1;
  for (int i = 0; i < BUS_REG_MAX; i++) {
    if (bus_reg[i] == bus) {
      EXIT_CRITICAL_FROM_ISR(s);
      return; // already known (v_bus_init called again on the same bus)
    }
    if (!bus_reg[i] && free_slot < 0)
      free_slot = i;
  }
  if (free_slot >= 0)
    bus_reg[free_slot] = bus;
  EXIT_CRITICAL_FROM_ISR(s);
}

static v_bus_topic_t *topic_by_name(const char *name) {
  for (int i = 0; i < BUS_REG_MAX; i++) {
    if (!bus_reg[i])
      continue;
    for (v_bus_topic_t *t = bus_reg[i]->topics; t; t = t->next)
      if (name_eq(t->name, name))
        return t;
  }
  return 0;
}

typedef struct {
  v_bus_topic_t *topic;
  v_bus_sub_t sub; // used only when the handle was opened V_BUS_RD
  // Blocking recv (B6). Static storage, so arming costs no heap and a handle
  // that dies takes its semaphore with it.
  StaticSemaphore_t sem_store;
  uint8_t used;
  uint8_t flags;
} bus_handle_t;

static bus_handle_t bus_handles[VAIOS_BUS_MAX_OPEN];

static int bus_fd_close(void *priv) {
  bus_handle_t *h = (bus_handle_t *)priv;
  if (h->flags & V_BUS_RD) {
    // Ordering-defensive: publish and unsubscribe serialise on the same
    // critical section, so no signal can reach a half-removed subscription
    // anyway — but the handle's semaphore storage is reused by the next open,
    // and clearing first states that intent rather than relying on the lock.
    h->sub.notify = 0;
    v_bus_unsubscribe(&h->sub); // releases its claim on unread messages
  }
  h->used = 0;
  return 0;
}
static const v_file_ops bus_fd_ops = {
    .read = NULL, .write = NULL, .close = bus_fd_close};

int v_bus_open(const char *name, int flags) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc2(SYS_bus_open, (uintptr_t)name, (uint32_t)flags);
#endif
  if (!name || !(flags & (V_BUS_RD | V_BUS_WR)))
    return V_BUS_EINVAL;
  v_bus_topic_t *t = topic_by_name(name);
  if (!t)
    return V_BUS_EINVAL; // no such topic on any initialised bus

  bus_handle_t *h = 0;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  for (int i = 0; i < VAIOS_BUS_MAX_OPEN && !h; i++)
    if (!bus_handles[i].used)
      h = &bus_handles[i];
  if (h) {
    h->used = 1;
    h->topic = t;
    h->flags = (uint8_t)flags;
  }
  EXIT_CRITICAL_FROM_ISR(s);
  if (!h)
    return V_BUS_EBUSY; // no free handle (VAIOS_BUS_MAX_OPEN)

  if (flags & V_BUS_RD) {
    int r = v_bus_subscribe(t, &h->sub);
    if (r != VA_PASS) { // e.g. a pipe topic that already has its one reader
      h->used = 0;
      return r;
    }
    // Arm blocking mode: v_bus_wait sleeps on this, publish signals it. Armed
    // for every reader because the cost when nobody waits is one NULL test per
    // subscriber per publish.
    h->sub.notify = v_semaphore_create_binary_static(&h->sem_store);
  }
  int fd = v_fd_alloc(&bus_fd_ops, h);
  if (fd < 0) {
    bus_fd_close(h);
    return V_BUS_EBUSY; // no free descriptor in this task
  }
  return fd;
}

int v_bus_send(int fd, const void *payload, uint16_t len) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc3(SYS_bus_send, (uint32_t)fd, (uintptr_t)payload, len);
#endif
  bus_handle_t *h = (bus_handle_t *)v_fd_obj(fd, &bus_fd_ops);
  if (!h || !(h->flags & V_BUS_WR))
    return V_BUS_EINVAL;
  return v_bus_publish(h->topic, payload, len);
}

int v_bus_recv(int fd, v_bus_rx_t *rx) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc2(SYS_bus_recv, (uint32_t)fd, (uintptr_t)rx);
#endif
  if (!rx)
    return V_BUS_EINVAL;
  bus_handle_t *h = (bus_handle_t *)v_fd_obj(fd, &bus_fd_ops);
  if (!h || !(h->flags & V_BUS_RD))
    return V_BUS_EINVAL;
  return v_bus_pop(&h->sub, rx->buf, rx->cap, &rx->len, &rx->missed);
}

// Has this subscription got something to read right now? A pipe reader's
// position lives in the ring, a queued topic's in its cursor.
static int sub_ready(v_bus_sub_t *sub) {
  if (sub->topic && sub->topic->cfg.pipe)
    return pipe_next(sub) != 0;
  return sub->cursor != V_BUS_NIL;
}

int v_bus_wait(int fd, uint32_t ticks) {
#if VAIOS_SYSCALL_SVC
  if (v_in_thread_mode())
    return v_svc2(SYS_bus_wait, (uint32_t)fd, ticks);
#endif
  bus_handle_t *h = (bus_handle_t *)v_fd_obj(fd, &bus_fd_ops);
  if (!h || !(h->flags & V_BUS_RD) || !h->sub.notify)
    return V_BUS_EINVAL;
  if (sub_ready(&h->sub))
    return VA_PASS; // already there: don't sleep on a signal already consumed
  return v_semaphore_take(h->sub.notify, ticks);
}

int v_bus_recv_wait(int fd, v_bus_rx_t *rx, uint32_t ticks) {
  // Composed of trapping calls, so it needs no trampoline of its own — and the
  // read is a separate step from the wait, which is what keeps the caller's
  // buffer out of kernel state while it sleeps.
  uint32_t start = v_get_ticks();
  for (;;) {
    int r = v_bus_recv(fd, rx);
    if (r != VA_FAIL)
      return r; // a message, or a real error (EINVAL/EMSGSIZE/...)
    uint32_t spent = v_get_ticks() - start;
    if (spent >= ticks)
      return VA_FAIL; // timed out (ticks == 0 lands here: plain v_bus_recv)
    if (v_bus_wait(fd, ticks - spent) != VA_PASS)
      return VA_FAIL;
    // Woken: loop and read. A signal whose message was evicted before we ran
    // just costs another turn of this loop, bounded by the deadline.
  }
}
#endif // VAIOS_DEVFS
