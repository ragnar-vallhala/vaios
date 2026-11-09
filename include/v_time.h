#ifndef VAIOS_V_TIME_H
#define VAIOS_V_TIME_H
#include <stdint.h>

/* Wall-clock date and time, as the port's RTC keeps it (plain binary). */
typedef struct {
  uint16_t year; /* full year, e.g. 2026 */
  uint8_t month; /* 1..12 */
  uint8_t day;   /* 1..31 */
  uint8_t hour;  /* 0..23 */
  uint8_t minute;
  uint8_t second;
} v_datetime_t;

#endif // VAIOS_V_TIME_H
