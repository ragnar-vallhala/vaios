// Bus IPC subsystem — see include/bus.h and docs/plans/bus-subsystem.md.
//
// B1: the block-pool allocator. Free blocks form a stack threaded through
// desc[].next, so taking or returning n blocks is O(n) whatever the pool size.
// Every mutation runs under ENTER_CRITICAL_FROM_ISR: short, bounded, and usable
// from the single-producer ISR publish path (plan §6.3, §7).
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
#include "bus.h"
#include "port.h"  // ENTER/EXIT_CRITICAL_FROM_ISR
#include "utils.h" // v_memcpy

// Header at the start of a message's first block.
typedef struct {
  uint32_t seq;  // assigned at link time: monotonic in queue order
  uint16_t link; // next (newer) message in the topic, or V_BUS_NIL
  uint16_t len;  // payload bytes
  uint16_t refs; // subscribers that still have to read it
  uint16_t pad;
} msg_hdr_t;
_Static_assert(sizeof(msg_hdr_t) == V_BUS_HDR_SIZE, "V_BUS_HDR_SIZE drift");

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
  return VA_PASS;
}

uint16_t v_bus_block_alloc(v_bus_t *bus, uint16_t n) {
  if (!bus || !n)
    return V_BUS_NIL;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  if (bus->free_count < n) { // all or nothing: never a partial chain
    EXIT_CRITICAL_FROM_ISR(s);
    return V_BUS_NIL;
  }
  uint16_t head = bus->free_head, idx = head;
  for (uint16_t k = 1;; k++) {
    bus->desc[idx].used = 1;
    if (k == n)
      break;
    idx = bus->desc[idx].next;
  }
  bus->free_head = bus->desc[idx].next; // the rest of the free stack
  bus->desc[idx].next = V_BUS_NIL;      // terminate the message chain
  bus->free_count = (uint16_t)(bus->free_count - n);
  EXIT_CRITICAL_FROM_ISR(s);
  return head;
}

int v_bus_block_free(v_bus_t *bus, uint16_t head) {
  if (!bus)
    return VA_FAIL;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  // Validate the whole chain before touching it: a double free or a stray
  // index must not splice a live block (or a cycle) into the free stack.
  uint16_t len = 0, tail = V_BUS_NIL;
  for (uint16_t i = head; i != V_BUS_NIL; i = bus->desc[i].next) {
    if (i >= bus->block_count || !bus->desc[i].used ||
        len == bus->block_count) {
      EXIT_CRITICAL_FROM_ISR(s);
      return VA_FAIL;
    }
    len++;
    tail = i;
  }
  if (!len) {
    EXIT_CRITICAL_FROM_ISR(s);
    return VA_FAIL;
  }
  for (uint16_t i = head; i != V_BUS_NIL; i = bus->desc[i].next) {
    bus->desc[i].used = 0;
    bus->desc[i].epoch++; // a reader still holding this index can tell
  }
  bus->desc[tail].next = bus->free_head; // push the chain onto the stack
  bus->free_head = head;
  bus->free_count = (uint16_t)(bus->free_count + len);
  EXIT_CRITICAL_FROM_ISR(s);
  return VA_PASS;
}

uint16_t v_bus_block_next(const v_bus_t *bus, uint16_t idx) {
  return idx < bus->block_count ? bus->desc[idx].next : V_BUS_NIL;
}

uint8_t *v_bus_block_data(v_bus_t *bus, uint16_t idx) {
  return idx < bus->block_count ? bus->blocks + (uint32_t)idx * bus->block_size
                                : (uint8_t *)0;
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
    used = (uint16_t)(used + bus->desc[i].used);
  return on_list == bus->free_count &&
                 used == bus->block_count - bus->free_count &&
                 topics_ok(bus)
             ? VA_PASS
             : VA_FAIL;
}

// --- Topics and messages (B2) ------------------------------------------------
// Unchecked block address for internal callers, whose indices come from the
// allocator, a cursor or a queue link (all in range by construction). The
// public v_bus_block_data keeps its bounds check.
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
  for (uint16_t b = msg; len; b = v_bus_block_next(bus, b), off = 0) {
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
  *topic = (v_bus_topic_t){.bus = bus,
                           .name = name,
                           .next = bus->topics,
                           .subs = 0,
                           .cfg = cfg ? *cfg : (v_bus_topic_cfg_t){0},
                           .head = V_BUS_NIL,
                           .tail = V_BUS_NIL};
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
  *sub = (v_bus_sub_t){.topic = topic,
                       .next = topic->subs,
                       .cursor = V_BUS_NIL,
                       .expect = topic->next_seq};
  topic->subs = sub;
  topic->nsubs++;
  EXIT_CRITICAL_FROM_ISR(s);
  return VA_PASS;
}

// Caller holds the critical section. Unlink the topic's oldest message and
// return it for freeing; its blocks leave the topic's count.
static uint16_t take_head(v_bus_topic_t *topic) {
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
  // Drop its claim on every message it hadn't read...
  for (uint16_t m = sub->cursor; m != V_BUS_NIL; m = hdr(bus, m)->link)
    hdr(bus, m)->refs--;
  // ...and free those nobody else is waiting for. Every reader goes in order,
  // so a message's refs never exceed a newer one's: the zeros are a run at
  // the head. ponytail: freed inside the critical section, O(queued blocks);
  // unsubscribe is rare and task-level. Hand them out after it if that bites.
  while (topic->head != V_BUS_NIL && !hdr(bus, topic->head)->refs)
    v_bus_block_free(bus, take_head(topic));
  EXIT_CRITICAL_FROM_ISR(s);
  sub->topic = 0;
  return VA_PASS;
}

// V_BUS_OVERWRITE: evict the oldest message; readers that hadn't read it move
// on to the next one (they see the gap as `missed` at their next pop).
static int evict_oldest(v_bus_topic_t *topic) {
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  if (topic->head == V_BUS_NIL) {
    EXIT_CRITICAL_FROM_ISR(s);
    return 0;
  }
  uint16_t link = hdr(topic->bus, topic->head)->link;
  for (v_bus_sub_t *x = topic->subs; x; x = x->next)
    if (x->cursor == topic->head)
      x->cursor = link;
  uint16_t msg = take_head(topic);
  v_bus_block_free(topic->bus, msg);
  EXIT_CRITICAL_FROM_ISR(s);
  return 1;
}

int v_bus_publish(v_bus_topic_t *topic, const void *payload, uint16_t len) {
  if (!topic || !topic->bus || (len && !payload))
    return V_BUS_EINVAL;
  v_bus_t *bus = topic->bus;
  uint32_t n = blocks_for(bus, len);
  if (n > bus->block_count)
    return V_BUS_EINVAL; // could never fit
  if (!topic->nsubs)
    return VA_PASS; // nobody to deliver to: keep nothing
  uint16_t msg = v_bus_block_alloc(bus, (uint16_t)n);
  if (msg == V_BUS_NIL && topic->cfg.overflow == V_BUS_OVERWRITE &&
      bus->free_count + topic->blocks >= n) { // evicting can make room
    while (msg == V_BUS_NIL && evict_oldest(topic))
      msg = v_bus_block_alloc(bus, (uint16_t)n);
  }
  if (msg == V_BUS_NIL)
    return VA_FAIL; // dropped

  // Fill it in while nobody can see it yet.
  msg_hdr_t *h = hdr(bus, msg);
  h->link = V_BUS_NIL;
  h->len = len;
  copy_payload(bus, msg, (void *)payload, len, 1);

  // Link: from here on it's visible. seq and refs are set under the same
  // critical section as subscribe, so refs counts exactly the subscribers
  // whose cursor can reach this message.
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  h->seq = topic->next_seq++;
  h->refs = topic->nsubs;
  if (topic->tail != V_BUS_NIL)
    hdr(bus, topic->tail)->link = msg;
  else
    topic->head = msg;
  topic->tail = msg;
  topic->blocks = (uint16_t)(topic->blocks + n);
  for (v_bus_sub_t *x = topic->subs; x; x = x->next)
    if (x->cursor == V_BUS_NIL) // caught up: this is its next message
      x->cursor = msg;
  EXIT_CRITICAL_FROM_ISR(s);
  return VA_PASS;
}

#ifdef VAIOS_HOST_TEST
// Host-test seam: runs between pop's copy and its commit, so a test can evict
// the message mid-read (on target that is an ISR publish preempting the pop).
void (*v_bus_test_mid_pop)(void);
#endif

int v_bus_pop(v_bus_sub_t *sub, void *out, uint16_t cap, uint16_t *out_len,
              uint32_t *missed) {
  if (!sub || !sub->topic || !out_len || (cap && !out))
    return V_BUS_EINVAL;
  v_bus_topic_t *topic = sub->topic;
  v_bus_t *bus = topic->bus;

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

    int reclaim = 0;
    s = ENTER_CRITICAL_FROM_ISR();
    if (bus->desc[msg].epoch != epoch) {
      // Evicted while we copied: `out` may be torn. The eviction already moved
      // our cursor on; read that one instead.
      EXIT_CRITICAL_FROM_ISR(s);
      continue;
    }
    msg_hdr_t *h = hdr(bus, msg);
    if (missed)
      *missed = h->seq - sub->expect; // overwritten before we got to them
    sub->expect = h->seq + 1u;
    sub->cursor = h->link; // V_BUS_NIL if we're caught up
    if (--h->refs == 0) {
      // Every reader reads in order, so the last reference always drops on
      // the oldest message.
      take_head(topic);
      reclaim = 1;
    }
    EXIT_CRITICAL_FROM_ISR(s);
    if (reclaim)
      v_bus_block_free(bus, msg);
    return VA_PASS;
  }
}

// v_bus_check, per topic: head..tail is a well-formed chain of live messages,
// each still owed to at least one subscriber, their blocks add up to the
// topic's count, and every cursor points into the chain.
static int topics_ok(const v_bus_t *bus) {
  const v_bus_t *b = bus;
  for (const v_bus_topic_t *t = bus->topics; t; t = t->next) {
    if ((t->head == V_BUS_NIL) != (t->tail == V_BUS_NIL))
      return 0;
    uint16_t last = V_BUS_NIL, n = 0;
    uint32_t blocks = 0;
    for (uint16_t m = t->head; m != V_BUS_NIL; m = hdr(b, m)->link) {
      if (m >= bus->block_count || !bus->desc[m].used || !hdr(b, m)->refs ||
          ++n > bus->block_count)
        return 0;
      blocks += blocks_for(bus, hdr(b, m)->len);
      last = m;
    }
    if (last != t->tail || blocks != t->blocks)
      return 0;
    uint16_t nsubs = 0;
    for (const v_bus_sub_t *x = t->subs; x; x = x->next) {
      uint16_t m = t->head;
      while (m != V_BUS_NIL && m != x->cursor)
        m = hdr(b, m)->link;
      if (m != x->cursor || ++nsubs > t->nsubs) // off the chain, or list loops
        return 0;
    }
    if (nsubs != t->nsubs)
      return 0;
  }
  return 1;
}
