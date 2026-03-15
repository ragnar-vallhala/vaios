#ifndef VAIOS_H
#define VAIOS_H
#include <stdint.h>
void v_init(void);
void v_system_init(void);
void v_start(void);
void v_stop(void);
void v_delay(uint32_t ms);
#endif // !VAIOS_H
