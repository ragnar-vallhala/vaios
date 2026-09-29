#include "memory.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stddef.h>
#include <stdint.h>

#define MAX_ALLOC_COUNT 100
#define MAX_ALLOC_CYCLES 10000
#define ALLOC_NONE  0
#define ALLOC_HEAP  2
#define ALLOC_WEIGHT 2
static inline uint32_t nonzero_sz(uint32_t ticks) {
  uint32_t s = ticks % 64;
  return s ? s : 1;
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_init(&cfg);
  v_heap_memory_init();

  // Zero-initialized table so we never free garbage.
  void **arr = (void **)v_malloc(MAX_ALLOC_COUNT* sizeof(void *));
  uint8_t alloc_type[MAX_ALLOC_COUNT] = {0};
  uint8_t alloc_count = 0;

  uint32_t cycles = 0;
  while (1) {
    if (++cycles >= MAX_ALLOC_CYCLES) break;

    uint32_t ticks = v_get_ticks();
      // ---------------- HEAP ----------------
      if (ticks % ALLOC_WEIGHT) {
        // Alloc (push)
        if (alloc_count < MAX_ALLOC_COUNT) {
          uint32_t sz = nonzero_sz(ticks);
          void *p = (void *)v_malloc(sz);
          if (p) {
            arr[alloc_count] = p;
            alloc_type[alloc_count] = ALLOC_HEAP;
            v_log(LOG_INFO,
                  "Heap allocation success | %d bytes allocated at 0x%x | %d/%d allocations live",
                  sz, p, alloc_count + 1, MAX_ALLOC_COUNT);
            alloc_count++;
          } else {
            v_log(LOG_ERROR, "Heap allocation failed");
          }
        } else {
          v_log(LOG_DEBUG, "Max allocation count reached!");
        }
      } else {
        // Free ONE heap block (swap-and-pop)
        if (alloc_count == 0) {
          v_log(LOG_DEBUG, "No heap alloc to free.");
        } else {
          int idx = -1;
          for (int i = 0; i < alloc_count; i++) {
            if (alloc_type[i] == ALLOC_HEAP) { idx = i; break; }
          }
          if (idx >= 0) {
            v_free(arr[idx]);

            int last = alloc_count - 1;
            if (idx != last) {
              arr[idx] = arr[last];
              alloc_type[idx] = alloc_type[last];
            }
            arr[last] = NULL;
            alloc_type[last] = ALLOC_NONE;
            alloc_count--;

            v_log(LOG_INFO, "Freed heap block (slot %d), live=%d", idx, alloc_count);
          } else {
            v_log(LOG_DEBUG, "No heap alloc to free.");
          }
        }
      }
      v_log(LOG_DEBUG, "%d/%d bytes used from heap.",
            v_get_heap_allocation_size(), v_get_heap_size());
    }
  while(1){};
}

