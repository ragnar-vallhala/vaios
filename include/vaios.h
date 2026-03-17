#ifndef VAIOS_H
#define VAIOS_H
#include <stdint.h>
typedef struct {
  uint8_t internal_clock_setup; // 0: if vaios default clock setup is used else
                                // 1 if user setups own clock
  uint8_t internal_sd_card_setup; // 0: if vaios default sd card setup is is
                                  // used else 1 if user setups own sd card
} vaios_init_config_t;
void v_init(vaios_init_config_t *cfg);
void v_system_init(vaios_init_config_t *cfg);
void v_start(void);
void v_stop(void);
void v_delay(uint32_t ms);
#endif // !VAIOS_H
