#include "utils.h"
#include "config.h"
#include "semihosting.h"
#include <stdarg.h>
#include <stdint.h>

#ifdef NAVHAL
#include "navhal.h"
#endif

// base print
void print(const char *str) {
#ifdef NAVHAL
  uart2_write(str);
#else
  sh_write0(str);
#endif
}

// helper: reverse string (unchanged)
static void reverse(char *str, int len) {
  int i = 0, j = len - 1;
  while (i < j) {
    char tmp = str[i];
    str[i] = str[j];
    str[j] = tmp;
    i++;
    j--;
  }
}

// helper: unsigned integer to string
static int utoa_simple(uint64_t value, char *buf, int base) {
  int i = 0;

  if (value == 0) {
    buf[i++] = '0';
    buf[i] = '\0';
    return i;
  }

  while (value > 0) {
    int rem = value % base;
    buf[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
    value /= base;
  }

  buf[i] = '\0';
  reverse(buf, i);
  return i;
}

// helper: signed int wrapper (uses utoa_simple)
static int itoa_simple(int64_t value, char *buf, int base) {
  if (value < 0 && base == 10) {
    int len = utoa_simple((uint64_t)(-value), buf + 1, base);
    buf[0] = '-';
    return len + 1;
  } else {
    return utoa_simple((uint64_t)value, buf, base);
  }
}

void vaprint_fmt(const char *fmt, va_list args) {
  char buffer[64];
  memset(buffer, 0, sizeof(buffer));

  for (const char *p = fmt; *p; p++) {
    if (*p != '%') {
      char tmp[2] = {*p, '\0'};
      print(tmp);
      continue;
    }

    p++; // skip '%'
    int width = 0;
    int zero_pad = 0;

    // Parse flags
    if (*p == '0') {
      zero_pad = 1;
      p++;
    }

    // Parse width
    while (*p >= '0' && *p <= '9') {
      width = width * 10 + (*p - '0');
      p++;
    }

    // Handle specifiers
    switch (*p) {
    case 'd': // signed int
    {
      int v = va_arg(args, int);
      itoa_simple(v, buffer, 10);
      int len = strlen(buffer);
      for (int i = len; i < width; i++) {
        print(zero_pad ? "0\0" : " \0");
      }
      print(buffer);
      memset(buffer, 0, sizeof(buffer));
      break;
    }
    case 'u': // unsigned int
    {
      uint32_t v = va_arg(args, uint32_t);
      utoa_simple(v, buffer, 10);
      int len = strlen(buffer);
      for (int i = len; i < width; i++) {
        print(zero_pad ? "0\0" : " \0");
      }
      print(buffer);
      memset(buffer, 0, sizeof(buffer));
      break;
    }
    case 'l': {
      p++;
      if (*p == 'u') // %lu
      {
        unsigned long v = va_arg(args, unsigned long);
        utoa_simple(v, buffer, 10);
      } else if (*p == 'd') // %ld
      {
        long v = va_arg(args, long);
        itoa_simple(v, buffer, 10);
      } else if (*p == 'l') // %llu / %lld
      {
        p++;
        if (*p == 'u') // %llu
        {
          unsigned long long v = va_arg(args, unsigned long long);
          utoa_simple(v, buffer, 10);
        } else if (*p == 'd') // %lld
        {
          long long v = va_arg(args, long long);
          itoa_simple(v, buffer, 10);
        }
      }
      int len = strlen(buffer);
      for (int i = len; i < width; i++) {
        print(zero_pad ? "0\0" : " \0");
      }
      print(buffer);
      memset(buffer, 0, sizeof(buffer));
      break;
    }
    case 'x': // 32-bit hex lowercase
    {
      uint32_t v = va_arg(args, uint32_t);
      memset(buffer, 0, sizeof(buffer));
      utoa_simple(v, buffer, 16);
      int len = strlen(buffer);
      for (int i = len; i < width; i++)
        print(zero_pad ? "0\0" : " \0");
      print(buffer);
      break;
    }
    case 'X': // 32/64-bit hex uppercase
    {
      uint64_t v = va_arg(args, uint32_t);
      memset(buffer, 0, sizeof(buffer));
      utoa_simple(v, buffer, 16);
      // convert to uppercase
      for (int i = 0; buffer[i] != '\0'; i++) {
        if (buffer[i] >= 'a' && buffer[i] <= 'f')
          buffer[i] -= 32;
      }
      int len = strlen(buffer);
      for (int i = len; i < width; i++)
        print(zero_pad ? "0\0" : " \0");
      print(buffer);
      memset(buffer, 0, sizeof(buffer));
      break;
    }
    case 'c': {
      char c = (char)va_arg(args, int);
      char tmp[2] = {c, '\0'};
      print(tmp);
      break;
    }
    case 's': {
      char *s = va_arg(args, char *);
      print(s);
      break;
    }
    case '%': {
      print("%");
      break;
    }
    default: {
      print("%");
      char tmp[2] = {*p, '\0'};
      print(tmp);
      break;
    }
    }
  }
}

void print_fmt(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vaprint_fmt(fmt, args);
  va_end(args);
}

void v_log(Log_Type type, const char *msg, ...) {
#if LOGGING_ENABLED == 1
  const char *typeName;
  const char *typeColor;
  switch (type) {
  case LOG_TRACE:
    typeName = "TRACE";
    typeColor = COLOR_TRACE;
    break;
  case LOG_DEBUG:
    typeName = "DEBUG";
    typeColor = COLOR_DEBUG;
    break;
  case LOG_INFO:
    typeName = "INFO ";
    typeColor = COLOR_INFO;
    break;
  case LOG_WARN:
    typeName = "WARN ";
    typeColor = COLOR_WARN;
    break;
  case LOG_ERROR:
    typeName = "ERROR";
    typeColor = COLOR_ERROR;
    break;
  case LOG_FATAL:
    typeName = "FATAL";
    typeColor = COLOR_FATAL;
    break;
  default:
    typeName = "UNK??";
    typeColor = COLOR_UNKNOWN;
    break;
  }
  va_list args;
  va_start(args, msg);
  print_fmt("%s[%s %u]%s ", typeColor, typeName, v_get_ticks(), COLOR_RESET);
  vaprint_fmt(msg, args);
  print_fmt("\r\n");
#endif
}

volatile uint32_t systick_count = 0;
#define ICSR (*(volatile uint32_t *)0xE000ED04)
#define ICSR_PENDSVSET (1 << 28) // Set this to trigger PendSV
#define ICSR_PENDSVCLR (1 << 27) // Clear PendSV
extern uint8_t scheduler_running;
void SysTick_Handler(void) {
  systick_count++;
  if ((scheduler_running == 123) && (systick_count % TIME_SLICE == 0))
    ICSR |= ICSR_PENDSVSET; // Trigger PENDSV
}

uint32_t v_get_ticks(void) { return systick_count; }

void *memset(void *s, int c, unsigned int n) {
  if (n == 0)
    return s;
  uint8_t val = (uint8_t)c;
  uint8_t *buf = (uint8_t *)s;
  while (--n) {
    buf[n] = val;
  }
  buf[0] = val;
  return s;
}

uint32_t strlen(const char *s) {
  if (!s)
    return 0;
  uint32_t count = 0;
  while (s[count] != '\0' && count != ~(0)) {
    count++;
  }
  return count;
}
