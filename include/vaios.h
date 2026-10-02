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

/* The boot banner (version, build revision and time, builder and compiler,
   target), \r\n-terminated lines; and the enabled kernel features, each
   space-led. Both in flash, so any task may print them. */
extern const char v_banner[];
extern const char v_features[];
void v_delay(uint32_t ms);
#endif // !VAIOS_H
