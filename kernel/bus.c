// Bus IPC subsystem — see include/bus.h and docs/plans/bus-subsystem.md.
//
// B1: the block-pool allocator. Free blocks form a stack threaded through
// desc[].next, so taking or returning n blocks is O(n) whatever the pool size.
// Every mutation runs under ENTER_CRITICAL_FROM_ISR: short, bounded, and usable
// from the single-producer ISR publish path (plan §6.3, §7).
#include "bus.h"
#include "port.h" // ENTER/EXIT_CRITICAL_FROM_ISR

int v_bus_init(v_bus_t *bus, void *blocks, v_bus_desc_t *desc,
               uint16_t block_size, uint16_t block_count) {
  if (!bus || !blocks || !desc || !block_size || !block_count ||
      block_count >= V_BUS_NIL)
    return VA_FAIL;
  for (uint16_t i = 0; i < block_count; i++) {
    desc[i].next = (uint16_t)(i + 1u < block_count ? i + 1u : V_BUS_NIL);
    desc[i].used = 0;
  }
  *bus = (v_bus_t){.blocks = (uint8_t *)blocks,
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
  for (uint16_t i = head; i != V_BUS_NIL; i = bus->desc[i].next)
    bus->desc[i].used = 0;
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
                 used == bus->block_count - bus->free_count
             ? VA_PASS
             : VA_FAIL;
}
