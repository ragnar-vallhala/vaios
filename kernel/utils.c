#include "utils.h"
#include "config.h"
#include "semihosting.h"
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include "port.h"
#ifdef NAVHAL
#include "navhal.h"
#endif

//---------------
// This is the shitiest code of the whole project.
// Strings are really hard to format without heap allocation or
// complex state machines. This is a very basic implementation that
// supports a few common format specifiers.
// Still in future if I ever come accross it (I will have to) then I will make it better
// But for now it will stay the same I have no more courage
//----------------

void *memset(void *s, int c, unsigned int n);
void *memcpy(void *dest, const void *src, unsigned int n);
uint32_t strlen(const char *s);

// Basic print function (to UART or semihosting)
void print(const char *str)
{
#ifdef NAVHAL
  uart2_write(str);
#else
  sh_write0(str);
#endif
}

// helper: reverse string in place
static void reverse(char *str, int len)
{
  int i = 0, j = len - 1;
  while (i < j)
  {
    char tmp = str[i];
    str[i] = str[j];
    str[j] = tmp;
    i++;
    j--;
  }
}

// helper: unsigned integer to string
static int utoa_simple(uint64_t value, char *buf, int base)
{
  int i = 0;

  if (value == 0)
  {
    buf[i++] = '0';
    buf[i] = '\0';
    return i;
  }

  while (value > 0)
  {
    int rem = value % base;
    buf[i++] = (rem > 9) ? (rem - 10) + 'a' : rem + '0';
    value /= base;
  }

  buf[i] = '\0';
  reverse(buf, i);
  return i;
}

// helper: signed int wrapper (uses utoa_simple)
static int itoa_simple(int64_t value, char *buf, int base)
{
  if (value < 0 && base == 10)
  {
    int len = utoa_simple((uint64_t)(-value), buf + 1, base);
    buf[0] = '-';
    return len + 1;
  }
  else
  {
    return utoa_simple((uint64_t)value, buf, base);
  }
}
void vaprint_fmt(const char *fmt, va_list args)
{
  char buffer[32];
  memset(buffer, 0, sizeof(buffer));

  for (const char *p = fmt; *p; p++)
  {
    if (*p != '%')
    {
      char tmp[2] = {*p, '\0'};
      print(tmp);
      continue;
    }

    p++; // skip '%'
    int width = 0;
    int zero_pad = 0;

    // Parse flags
    if (*p == '0')
    {
      zero_pad = 1;
      p++;
    }

    // Parse width
    while (*p >= '0' && *p <= '9')
    {
      width = width * 10 + (*p - '0');
      p++;
    }

    // Handle specifiers
    switch (*p)
    {
    case 'd': // signed int
    {
      int v = va_arg(args, int);
      itoa_simple(v, buffer, 10);
      int len = strlen(buffer);
      for (int i = len; i < width; i++)
      {
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
      for (int i = len; i < width; i++)
      {
        print(zero_pad ? "0\0" : " \0");
      }
      print(buffer);
      memset(buffer, 0, sizeof(buffer));
      break;
    }
    case 'f':
    { // floating point
      double v = va_arg(args, double);

      // Default precision: 6 decimal places
      int precision = 6;
      long long int_part = (long long)v;
      double frac_part = v - (double)int_part;

      if (frac_part < 0)
        frac_part = -frac_part; // handle negative numbers

      // Print integer part
      itoa_simple(int_part, buffer, 10);
      print(buffer);
      print(".");

      // Print fractional part
      for (int i = 0; i < precision; i++)
      {
        frac_part *= 10.0;
        int digit = (int)frac_part;
        char tmp[2] = {'0' + digit, '\0'};
        print(tmp);
        frac_part -= digit;
      }

      memset(buffer, 0, sizeof(buffer));
      break;
    }

    case 'l':
    {
      p++;
      if (*p == 'u') // %lu
      {
        unsigned long v = va_arg(args, unsigned long);
        utoa_simple(v, buffer, 10);
      }
      else if (*p == 'd') // %ld
      {
        long v = va_arg(args, long);
        itoa_simple(v, buffer, 10);
      }
      else if (*p == 'l') // %llu / %lld
      {
        p++;
        if (*p == 'u') // %llu
        {
          unsigned long long v = va_arg(args, unsigned long long);
          utoa_simple(v, buffer, 10);
        }
        else if (*p == 'd') // %lld
        {
          long long v = va_arg(args, long long);
          itoa_simple(v, buffer, 10);
        }
      }
      int len = strlen(buffer);
      for (int i = len; i < width; i++)
      {
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
      for (int i = 0; buffer[i] != '\0'; i++)
      {
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
    case 'c':
    {
      char c = (char)va_arg(args, int);
      char tmp[2] = {c, '\0'};
      print(tmp);
      break;
    }
    case 's':
    {
      char *s = va_arg(args, char *);
      print(s);
      break;
    }
    case '%':
    {
      print("%");
      break;
    }
    default:
    {
      print("%");
      char tmp[2] = {*p, '\0'};
      print(tmp);
      break;
    }
    }
  }
}

int vaprint_fmt_buf(char *out, size_t out_size, const char *fmt, va_list args)
{
  size_t pos = 0;
  char buffer[32];

  for (const char *p = fmt; *p && pos < out_size - 1; p++)
  {
    if (*p != '%')
    {
      out[pos++] = *p;
      continue;
    }

    p++; // skip '%'
    int width = 0;
    int zero_pad = 0;

    // Parse flags
    if (*p == '0')
    {
      zero_pad = 1;
      p++;
    }

    // Parse width
    while (*p >= '0' && *p <= '9')
    {
      width = width * 10 + (*p - '0');
      p++;
    }

    // Handle specifiers
    switch (*p)
    {
    case 'd':
    {
      int v = va_arg(args, int);
      itoa_simple(v, buffer, 10);
      int len = strlen(buffer);
      for (int i = len; i < width && pos < out_size - 1; i++)
        out[pos++] = zero_pad ? '0' : ' ';
      for (int i = 0; buffer[i] && pos < out_size - 1; i++)
        out[pos++] = buffer[i];
      break;
    }
    case 'u':
    {
      uint32_t v = va_arg(args, uint32_t);
      utoa_simple(v, buffer, 10);
      int len = strlen(buffer);
      for (int i = len; i < width && pos < out_size - 1; i++)
        out[pos++] = zero_pad ? '0' : ' ';
      for (int i = 0; buffer[i] && pos < out_size - 1; i++)
        out[pos++] = buffer[i];
      break;
    }
    case 'f':
    {
      double v = va_arg(args, double);
      int precision = 6;
      long long int_part = (long long)v;
      double frac_part = v - (double)int_part;
      if (frac_part < 0)
        frac_part = -frac_part;

      itoa_simple(int_part, buffer, 10);
      for (int i = 0; buffer[i] && pos < out_size - 1; i++)
        out[pos++] = buffer[i];
      out[pos++] = '.';

      for (int i = 0; i < precision && pos < out_size - 1; i++)
      {
        frac_part *= 10.0;
        int digit = (int)frac_part;
        out[pos++] = '0' + digit;
        frac_part -= digit;
      }
      break;
    }
    case 'l':
    {
      p++;
      if (*p == 'u')
      {
        unsigned long v = va_arg(args, unsigned long);
        utoa_simple(v, buffer, 10);
      }
      else if (*p == 'd')
      {
        long v = va_arg(args, long);
        itoa_simple(v, buffer, 10);
      }
      else if (*p == 'l')
      {
        p++;
        if (*p == 'u')
        {
          unsigned long long v = va_arg(args, unsigned long long);
          utoa_simple(v, buffer, 10);
        }
        else if (*p == 'd')
        {
          long long v = va_arg(args, long long);
          itoa_simple(v, buffer, 10);
        }
      }

      int len = strlen(buffer);
      for (int i = len; i < width && pos < out_size - 1; i++)
        out[pos++] = zero_pad ? '0' : ' ';
      for (int i = 0; buffer[i] && pos < out_size - 1; i++)
        out[pos++] = buffer[i];
      break;
    }
    case 'x':
    {
      uint32_t v = va_arg(args, uint32_t);
      utoa_simple(v, buffer, 16);
      int len = strlen(buffer);
      for (int i = len; i < width && pos < out_size - 1; i++)
        out[pos++] = zero_pad ? '0' : ' ';
      for (int i = 0; buffer[i] && pos < out_size - 1; i++)
        out[pos++] = buffer[i];
      break;
    }
    case 'X':
    {
      uint32_t v = va_arg(args, uint32_t);
      utoa_simple(v, buffer, 16);
      // uppercase
      for (int i = 0; buffer[i]; i++)
        if (buffer[i] >= 'a' && buffer[i] <= 'f')
          buffer[i] -= 32;
      int len = strlen(buffer);
      for (int i = len; i < width && pos < out_size - 1; i++)
        out[pos++] = zero_pad ? '0' : ' ';
      for (int i = 0; buffer[i] && pos < out_size - 1; i++)
        out[pos++] = buffer[i];
      break;
    }
    case 'c':
    {
      char c = (char)va_arg(args, int);
      if (pos < out_size - 1)
        out[pos++] = c;
      break;
    }
    case 's':
    {
      char *s = va_arg(args, char *);
      for (int i = 0; s[i] && pos < out_size - 1; i++)
        out[pos++] = s[i];
      break;
    }
    case '%':
      if (pos < out_size - 1)
        out[pos++] = '%';
      break;
    default:
      if (pos < out_size - 1)
        out[pos++] = '%';
      if (pos < out_size - 1)
        out[pos++] = *p;
      break;
    }
  }

  out[pos] = '\0';
  return pos;
}

void print_fmt(const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vaprint_fmt(fmt, args);
  va_end(args);
}

static int string_equal(const char *a, const char *b)
{
  while (*a && *b)
  {
    if (*a != *b)
      return 0;
    a++;
    b++;
  }
  return (*a == 0 && *b == 0);
}

// Returns 1 if this module is allowed to log
static int module_allowed(const char *msg)
{
  if (!msg || msg[0] != '[')
    return 1; // no module specified, allow by default

  // Extract module name from [MODULE]
  int i = 1;
  char module[32];
  int module_len = 0;
  while (msg[i] != ']' && msg[i] != 0 && module_len < (int)(sizeof(module) - 1))
  {
    module[module_len++] = msg[i++];
  }
  module[module_len] = 0; // null terminate

  if (module_len == 0)
    return 1; // malformed, allow

  // Check for "ALL"
  int is_all = 1;
  for (i = 0; ALLOWED_MODULES[i] != 0 && i < 3; i++)
  {
    if (ALLOWED_MODULES[i] != "ALL"[i])
    {
      is_all = 0;
      break;
    }
  }
  if (is_all)
    return 1;

  // Check if module exists in ALLOWED_MODULES
  const char *p = ALLOWED_MODULES;
  int token_start = 0;
  int pos = 0;
  while (1)
  {
    char c = p[pos];
    if (c == ',' || c == 0)
    {
      // Compare module with token p[token_start .. pos-1]
      int match = 1;
      int k;
      for (k = 0; k < module_len; k++)
      {
        if (k >= (pos - token_start) || module[k] != p[token_start + k])
        {
          match = 0;
          break;
        }
      }
      if (match && k == (pos - token_start))
        return 1; // found

      if (c == 0)
        break; // end of string

      token_start = pos + 1; // start next token
    }
    pos++;
  }

  return 0; // not allowed
}

void v_log(Log_Type type, const char *msg, ...)
{
  if (type < MIN_LOG_LEVEL || !module_allowed(msg))
    return;

#if LOGGING_ENABLED == 1
#if BUFFERED_LOGGING == 1
  char formatted_msg[LOG_MSG_MAX_LEN];
  va_list args;
  va_start(args, msg);
  vaprint_fmt_buf(formatted_msg, sizeof(formatted_msg), msg, args);
  va_end(args);

  int next_head = (log_head + 1) % LOG_BUFFER_SIZE;
  if (next_head != log_tail) // buffer not full
  {
    log_buffer[log_head].type = type;
    memcpy(log_buffer[log_head].msg, formatted_msg, LOG_MSG_MAX_LEN - 1);
    // strncpy(log_buffer[log_head].msg, msg, LOG_MSG_MAX_LEN - 1);
    log_buffer[log_head].msg[LOG_MSG_MAX_LEN - 1] = '\0';
    log_head = next_head;
  }
  else
  {
    // Optional: drop or overwrite oldest log
  }
#elif BUFFERED_LOGGING == 0
  const char *typeName;
  const char *typeColor;
  switch (type)
  {
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
#endif // BUFFERED_LOGGING
#endif // LOGGING_ENABLED
}

void v_log_flush(void)
{
#if LOGGING_ENABLED == 1
  while (log_tail != log_head)
  {
    LogEntry *entry = &log_buffer[log_tail];

    const char *typeName;
    const char *typeColor;

    switch (entry->type)
    {
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

    print_fmt("%s[%s %u]%s %s\r\n",
              typeColor, typeName, v_get_ticks(),
              COLOR_RESET, entry->msg);

    log_tail = (log_tail + 1) % LOG_BUFFER_SIZE;
  }
#endif
}

volatile uint32_t systick_count = 0;
#define ICSR (*(volatile uint32_t *)0xE000ED04)
#define ICSR_PENDSVSET (1 << 28) // Set this to trigger PendSV
#define ICSR_PENDSVCLR (1 << 27) // Clear PendSV
extern uint8_t scheduler_running;
void SysTick_Handler(void)
{
  systick_count++;
  if ((scheduler_running == 123) && (systick_count % TIME_SLICE == 0))
    ICSR |= ICSR_PENDSVSET; // Trigger PENDSV
}

uint32_t v_get_ticks(void) { return systick_count; }

void *memset(void *s, int c, unsigned int n)
{
  uint8_t *p = (uint8_t *)s;
  for (unsigned int i = 0; i < n; i++)
  {
    p[i] = (uint8_t)c;
  }
  return s;
}
void *memcpy(void *dest, const void *src, unsigned int n)
{
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;

  // Handle trivial case
  if (dest == src || n == 0)
    return dest;

  // If dest < src, copy forward
  if (d < s)
  {
    for (unsigned int i = 0; i < n; i++)
      d[i] = s[i];
  }
  else
  {
    // Overlapping regions — copy backwards
    for (unsigned int i = n; i != 0; i--)
      d[i - 1] = s[i - 1];
  }

  return dest;
}
uint32_t strlen(const char *s)
{
  if (!s)
    return 0;
  uint32_t count = 0;
  while (s[count] != '\0' && count != ~(0))
  {
    count++;
  }
  return count;
}
