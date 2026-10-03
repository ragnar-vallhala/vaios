#ifndef VAIOS_H
#define VAIOS_H
#include <stdint.h>
typedef struct {
  uint8_t internal_clock_setup;   // 1: vaios runs its internal clock (PLL) setup;
                                  // 0: skip it (caller already configured the clock)
  uint8_t internal_sd_card_setup; // 1: vaios runs its internal SD-card setup;
                                  // 0: skip it (caller already configured the SD card)
} vaios_init_config_t;
void v_init(vaios_init_config_t *cfg);
void v_system_init(vaios_init_config_t *cfg);

/* Render the boot banner -- logo, what this image is, a colour strip -- one
   \r\n-terminated line at a time through `put`. cpu_hz 0 leaves the clock out;
   uptime_s < 0 leaves uptime out. Const data and its own stack only, so any
   task may call it with its own writer. */
void v_banner_print(void (*put)(const char *line), uint32_t cpu_hz,
                    int32_t uptime_s);
void v_delay(uint32_t ms);
#endif // !VAIOS_H
