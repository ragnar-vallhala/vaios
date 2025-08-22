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

  for (const char *p = fmt; *p; p++) {
    if (*p != '%') {
      char tmp[2] = {*p, '\0'};
      print(tmp);
      continue;
    }

    p++; // skip '%'
    switch (*p) {
    case 'd': { // signed int
      int v = va_arg(args, int);
      itoa_simple(v, buffer, 10);
      print(buffer);
      break;
    }
    case 'u': { // unsigned int
      uint32_t v = va_arg(args, uint32_t);
      utoa_simple(v, buffer, 10);
      print(buffer);
      break;
    }
    case 'l': {
      p++;
      if (*p == 'u') { // %lu → 32-bit unsigned long
        unsigned long v = va_arg(args, unsigned long);
        utoa_simple((uint64_t)v, buffer, 10);
        print(buffer);
      } else if (*p == 'd') { // %ld → 32-bit signed long
        long v = va_arg(args, long);
        itoa_simple((int64_t)v, buffer, 10);
        print(buffer);
      } else if (*p == 'l') { // handle "ll"
        p++;
        if (*p == 'u') { // %llu → 64-bit unsigned
          unsigned long long v = va_arg(args, unsigned long long);
          utoa_simple((uint64_t)v, buffer, 10);
          print(buffer);
        } else if (*p == 'd') { // %lld → 64-bit signed
          long long v = va_arg(args, long long);
          itoa_simple((int64_t)v, buffer, 10);
          print(buffer);
        } else {
          print("%ll");
          char tmp[2] = {*p, '\0'};
          print(tmp);
        }
      } else {
        print("%l");
        char tmp[2] = {*p, '\0'};
        print(tmp);
      }
      break;
    }

    case 'x': { // hex (32-bit)
      uint32_t v = va_arg(args, uint32_t);
      utoa_simple(v, buffer, 16);
      print(buffer);
      break;
    }
    case 'X': { // hex (64-bit)
      uint64_t v = va_arg(args, uint64_t);
      utoa_simple(v, buffer, 16);
      print(buffer);
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
      // unknown specifier → print literally
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

#ifndef NAVHAL
void SysTick_Handler(void) {
  systick_count++;
  // Toggle a variable, blink LED, or trigger PendSV here if you want
}

#endif

uint32_t v_get_ticks(void) {
#ifdef NAVHAL
  return (uint32_t)hal_get_tick();
#else
  return systick_count;
#endif
}
