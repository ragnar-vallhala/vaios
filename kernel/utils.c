#include "utils.h"
#include "atomic.h"
#include "config.h"
#include "port.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#ifdef NAVHAL
#include "navhal.h"
#endif

//---------------
// This is the shitiest code of the whole project.
// Strings are really hard to format without heap allocation or
// complex state machines. This is a very basic implementation that
// supports a few common format specifiers.
// Still in future if I ever come accross it (I will have to) then I will make
// it better But for now it will stay the same I have no more courage
//----------------
void *v_memset(void *s, int c, unsigned int n);
void *v_memcpy(void *dest, const void *src, unsigned int n);
uint32_t v_strlen(const char *s);

// Double buffering for log messages
static uint8_t log_buffer_storage1[LOG_BUFFER_STORAGE_SIZE];
static uint8_t log_buffer_storage2[LOG_BUFFER_STORAGE_SIZE];
static uint8_t *log_buffer_storage_current_writing = log_buffer_storage1;
static uint8_t *log_buffer_storage_current_reading = log_buffer_storage2;
static uint16_t log_buffer_storage_writing_head = 0;
static atomic_t log_buffer_storage_read_lock = {.counter = 0};
static uint16_t log_buffer_size_to_read = 0;
int v_strcmp(const char *s1, const char *s2) {
  while (*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }
  return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}
int v_strncmp(const char *s1, const char *s2, int n) {
  while (n > 0) {
    if (*s1 != *s2) {
      return *(const unsigned char *)s1 - *(const unsigned char *)s2;
    }
    if (*s1 == '\0') {
      return 0;
    }
    s1++;
    s2++;
    n--;
  }
  return 0;
}

float v_atof(const char *s) {
  float res = 0.0f;
  float fact = 1.0f;
  int point_seen = 0;
  int sign = 1;

  // Skip whitespace
  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' ||
         *s == '\v') {
    s++;
  }

  // Handle sign
  if (*s == '-') {
    sign = -1;
    s++;
  } else if (*s == '+') {
    s++;
  }

  while (*s) {
    if (*s == '.') {
      if (point_seen)
        break;
      point_seen = 1;
      s++;
      continue;
    }

    int d = *s - '0';
    if (d >= 0 && d <= 9) {
      if (point_seen) {
        fact /= 10.0f;
        res = res + (float)d * fact;
      } else {
        res = res * 10.0f + (float)d;
      }
    } else {
      break;
    }
    s++;
  }

  return res * (float)sign;
}
// Use safely only if DMA is enabled and you know what you are doing
void direct_dma_print(const uint8_t *bytes, uint32_t len) {
#if defined(_DMA_ENABLED) && defined(_UART_BACKEND_DMA)
  uart2_write_dma(bytes, len);
#endif
}

// Callback for DMA completion to release the read lock
void dma_tx_complete_callback(void) {
  atomic_set(&log_buffer_storage_read_lock, 0);
}

// Basic print function (to UART or semihosting)
void v_print(const char *str) {
#ifdef NAVHAL
  uart2_write_string(str);
#else
  sh_write0(str);
#endif
}

// Basic print function (to UART or semihosting)
void print(const char *str) { v_print(str); }
// helper: reverse string in place
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
  char out_buf[128];
  uint32_t out_pos = 0;
  char buffer[64];

#define FLUSH_OUT_BUF()                                                        \
  if (out_pos > 0) {                                                           \
    out_buf[out_pos] = '\0';                                                   \
    print(out_buf);                                                            \
    out_pos = 0;                                                               \
  }

#define PUT_CHAR_BUF(c)                                                        \
  {                                                                            \
    if (out_pos >= sizeof(out_buf) - 1)                                        \
      FLUSH_OUT_BUF();                                                         \
    out_buf[out_pos++] = (c);                                                  \
  }

#define PUT_STR_BUF(s)                                                         \
  {                                                                            \
    const char *_s = (s);                                                      \
    while (*_s)                                                                \
      PUT_CHAR_BUF(*_s++);                                                     \
  }

  for (const char *p = fmt; *p; p++) {
    if (*p != '%') {
      PUT_CHAR_BUF(*p);
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
      int len = v_strlen(buffer);
      for (int i = len; i < width; i++) {
        PUT_CHAR_BUF(zero_pad ? '0' : ' ');
      }
      PUT_STR_BUF(buffer);
      break;
    }
    case 'u': // unsigned int
    {
      uint32_t v = va_arg(args, uint32_t);
      utoa_simple(v, buffer, 10);
      int len = v_strlen(buffer);
      for (int i = len; i < width; i++) {
        PUT_CHAR_BUF(zero_pad ? '0' : ' ');
      }
      PUT_STR_BUF(buffer);
      break;
    }
    case 'f': { // floating point
      double v = va_arg(args, double);

      // Default precision: 6 decimal places
      int precision = 6;
      long long int_part = (long long)v;
      double frac_part = v - (double)int_part;

      if (frac_part < 0)
        frac_part = -frac_part; // handle negative numbers

      // Print integer part
      itoa_simple(int_part, buffer, 10);
      PUT_STR_BUF(buffer);
      PUT_CHAR_BUF('.');

      // Print fractional part
      for (int i = 0; i < precision; i++) {
        frac_part *= 10.0;
        int digit = (int)frac_part;
        PUT_CHAR_BUF('0' + digit);
        frac_part -= digit;
      }
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
      int len = v_strlen(buffer);
      for (int i = len; i < width; i++) {
        PUT_CHAR_BUF(zero_pad ? '0' : ' ');
      }
      PUT_STR_BUF(buffer);
      break;
    }
    case 'x': // 32-bit hex lowercase
    {
      uint32_t v = va_arg(args, uint32_t);
      utoa_simple(v, buffer, 16);
      int len = v_strlen(buffer);
      for (int i = len; i < width; i++)
        PUT_CHAR_BUF(zero_pad ? '0' : ' ');
      PUT_STR_BUF(buffer);
      break;
    }
    case 'X': // 32/64-bit hex uppercase
    {
      uint32_t v = va_arg(args, uint32_t);
      utoa_simple(v, buffer, 16);
      // convert to uppercase
      for (int i = 0; buffer[i] != '\0'; i++) {
        if (buffer[i] >= 'a' && buffer[i] <= 'f')
          buffer[i] -= 32;
      }
      int len = v_strlen(buffer);
      for (int i = len; i < width; i++)
        PUT_CHAR_BUF(zero_pad ? '0' : ' ');
      PUT_STR_BUF(buffer);
      break;
    }
    case 'c': {
      char c = (char)va_arg(args, int);
      PUT_CHAR_BUF(c);
      break;
    }
    case 's': {
      char *s = va_arg(args, char *);
      PUT_STR_BUF(s);
      break;
    }
    case '%': {
      PUT_CHAR_BUF('%');
      break;
    }
    default: {
      PUT_CHAR_BUF('%');
      PUT_CHAR_BUF(*p);
      break;
    }
    }
  }

  FLUSH_OUT_BUF();

#undef FLUSH_OUT_BUF
#undef PUT_CHAR_BUF
#undef PUT_STR_BUF
}

int vaprint_fmt_buf(char *out, size_t out_size, const char *fmt, va_list args) {
  size_t pos = 0;
  char buffer[32];

  for (const char *p = fmt; *p && pos < out_size - 1; p++) {
    if (*p != '%') {
      out[pos++] = *p;
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
    case 'd': {
      int v = va_arg(args, int);
      itoa_simple(v, buffer, 10);
      int len = v_strlen(buffer);
      for (int i = len; i < width && pos < out_size - 1; i++)
        out[pos++] = zero_pad ? '0' : ' ';
      for (int i = 0; buffer[i] && pos < out_size - 1; i++)
        out[pos++] = buffer[i];
      break;
    }
    case 'u': {
      uint32_t v = va_arg(args, uint32_t);
      utoa_simple(v, buffer, 10);
      int len = v_strlen(buffer);
      for (int i = len; i < width && pos < out_size - 1; i++)
        out[pos++] = zero_pad ? '0' : ' ';
      for (int i = 0; buffer[i] && pos < out_size - 1; i++)
        out[pos++] = buffer[i];
      break;
    }
    case 'f': {
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

      for (int i = 0; i < precision && pos < out_size - 1; i++) {
        frac_part *= 10.0;
        int digit = (int)frac_part;
        out[pos++] = '0' + digit;
        frac_part -= digit;
      }
      break;
    }
    case 'l': {
      p++;
      if (*p == 'u') {
        unsigned long v = va_arg(args, unsigned long);
        utoa_simple(v, buffer, 10);
      } else if (*p == 'd') {
        long v = va_arg(args, long);
        itoa_simple(v, buffer, 10);
      } else if (*p == 'l') {
        p++;
        if (*p == 'u') {
          unsigned long long v = va_arg(args, unsigned long long);
          utoa_simple(v, buffer, 10);
        } else if (*p == 'd') {
          long long v = va_arg(args, long long);
          itoa_simple(v, buffer, 10);
        }
      }

      int len = v_strlen(buffer);
      for (int i = len; i < width && pos < out_size - 1; i++)
        out[pos++] = zero_pad ? '0' : ' ';
      for (int i = 0; buffer[i] && pos < out_size - 1; i++)
        out[pos++] = buffer[i];
      break;
    }
    case 'x': {
      uint32_t v = va_arg(args, uint32_t);
      utoa_simple(v, buffer, 16);
      int len = v_strlen(buffer);
      for (int i = len; i < width && pos < out_size - 1; i++)
        out[pos++] = zero_pad ? '0' : ' ';
      for (int i = 0; buffer[i] && pos < out_size - 1; i++)
        out[pos++] = buffer[i];
      break;
    }
    case 'X': {
      uint32_t v = va_arg(args, uint32_t);
      utoa_simple(v, buffer, 16);
      // uppercase
      for (int i = 0; buffer[i]; i++)
        if (buffer[i] >= 'a' && buffer[i] <= 'f')
          buffer[i] -= 32;
      int len = v_strlen(buffer);
      for (int i = len; i < width && pos < out_size - 1; i++)
        out[pos++] = zero_pad ? '0' : ' ';
      for (int i = 0; buffer[i] && pos < out_size - 1; i++)
        out[pos++] = buffer[i];
      break;
    }
    case 'c': {
      char c = (char)va_arg(args, int);
      if (pos < out_size - 1)
        out[pos++] = c;
      break;
    }
    case 's': {
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

void print_fmt(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vaprint_fmt(fmt, args);
  va_end(args);
}

static int string_equal(const char *a, const char *b) {
  while (*a && *b) {
    if (*a != *b)
      return 0;
    a++;
    b++;
  }
  return (*a == 0 && *b == 0);
}

// Returns 1 if this module is allowed to log
static int module_allowed(const char *msg) {
  if (!msg || msg[0] != '[')
    return 1; // no module specified, allow by default

  // Extract module name from [MODULE]
  int i = 1;
  char module[32];
  int module_len = 0;
  while (msg[i] != ']' && msg[i] != 0 &&
         module_len < (int)(sizeof(module) - 1)) {
    module[module_len++] = msg[i++];
  }
  module[module_len] = 0; // null terminate

  if (module_len == 0)
    return 0; // malformed, disallow

  // Check for "ALL"
  if (string_equal(ALLOWED_MODULES, "ALL"))
    return 1;

  // Check if module exists in ALLOWED_MODULES
  // TODO: This is a very inefficient way to check for module existence.
  // TODO: Use a hash set or a trie to store the allowed modules for O(1)
  // lookup.
  const char *p = ALLOWED_MODULES;
  int token_start = 0;
  int pos = 0;
  while (1) {
    char c = p[pos];
    if (c == ',' || c == 0) {
      // Compare module with token p[token_start .. pos-1]
      int match = 1;
      int k;
      for (k = 0; k < module_len; k++) {
        if (k >= (pos - token_start) || module[k] != p[token_start + k]) {
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

void v_log(Log_Type type, const char *msg, ...) {
  if (type < MIN_LOG_LEVEL || !module_allowed(msg))
    return;

#if LOGGING_ENABLED == 1
#if BUFFERED_LOGGING == 1
  char formatted_msg[LOG_MSG_MAX_LEN];
  va_list args;
  va_start(args, msg);
  vaprint_fmt_buf(formatted_msg, sizeof(formatted_msg), msg, args);
  va_end(args);

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
    typeName = "INFO";
    typeColor = COLOR_INFO;
    break;
  case LOG_WARN:
    typeName = "WARN";
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
    typeName = "UNKNOWN";
    typeColor = COLOR_UNKNOWN;
    break;
  }

  char final_msg[LOG_MSG_MAX_LEN + 32];
  uint32_t final_len = 0;

  // Prepend color and tag: "COLOR[TAG] "
  uint32_t color_len = v_strlen(typeColor);
  v_memcpy(final_msg + final_len, typeColor, color_len);
  final_len += color_len;

  final_msg[final_len++] = '[';
  uint32_t tag_len = v_strlen(typeName);
  v_memcpy(final_msg + final_len, typeName, tag_len);
  final_len += tag_len;
  final_msg[final_len++] = ' ';
  char ticks_str[11];
  uint32_t ticks_len = utoa_simple(v_get_ticks(), ticks_str, 10);
  v_memcpy(final_msg + final_len, ticks_str, ticks_len);
  final_len += ticks_len;

  final_msg[final_len++] = ']';

  // Reset color after the tag
  uint32_t reset_len = v_strlen(COLOR_RESET);
  v_memcpy(final_msg + final_len, COLOR_RESET, reset_len);
  final_len += reset_len;

  final_msg[final_len++] = ' ';

  // Append original message
  uint32_t orig_msg_len = v_strlen(formatted_msg);
  if (final_len + orig_msg_len + 5 > sizeof(final_msg)) {
    orig_msg_len = sizeof(final_msg) - final_len - 5;
  }
  v_memcpy(final_msg + final_len, formatted_msg, orig_msg_len);
  final_len += orig_msg_len;

  ENTER_CRITICAL();
  // Check if current message fits in current writing buffer
  if (log_buffer_storage_writing_head + final_len + 3 >
      LOG_BUFFER_STORAGE_SIZE) {
    // Current buffer is full or doesn't have enough space
    // Wait for previous flush to finish before swapping
    if (atomic_get(&log_buffer_storage_read_lock)) {
      EXIT_CRITICAL();
      while (atomic_get(&log_buffer_storage_read_lock))
        ;
      ENTER_CRITICAL();
    }

    // Set size to read from current buffer
    log_buffer_size_to_read = log_buffer_storage_writing_head;

    // Swap buffers
    if (log_buffer_storage1 == log_buffer_storage_current_writing) {
      log_buffer_storage_current_writing = log_buffer_storage2;
      log_buffer_storage_current_reading = log_buffer_storage1;
    } else {
      log_buffer_storage_current_writing = log_buffer_storage1;
      log_buffer_storage_current_reading = log_buffer_storage2;
    }
    log_buffer_storage_writing_head = 0;
  }

  // Copy message + CRLF
  v_memcpy(log_buffer_storage_current_writing + log_buffer_storage_writing_head,
           final_msg, final_len);
  log_buffer_storage_writing_head += final_len;
  log_buffer_storage_current_writing[log_buffer_storage_writing_head++] = '\r';
  log_buffer_storage_current_writing[log_buffer_storage_writing_head++] = '\n';
  log_buffer_storage_current_writing[log_buffer_storage_writing_head] = '\0';

  EXIT_CRITICAL();
#elif BUFFERED_LOGGING == 0
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
#endif // BUFFERED_LOGGING
#endif // LOGGING_ENABLED
}

void v_log_flush(void) {
#if LOGGING_ENABLED == 1
#if BUFFERED_LOGGING == 1
  // If a flush is already in progress, just return
  if (atomic_get(&log_buffer_storage_read_lock))
    return;

  ENTER_CRITICAL();
  // If no data to read but there is data to write, swap buffers to flush
  if (log_buffer_size_to_read == 0 && log_buffer_storage_writing_head > 0) {
    log_buffer_size_to_read = log_buffer_storage_writing_head;
    if (log_buffer_storage1 == log_buffer_storage_current_writing) {
      log_buffer_storage_current_writing = log_buffer_storage2;
      log_buffer_storage_current_reading = log_buffer_storage1;
    } else {
      log_buffer_storage_current_writing = log_buffer_storage1;
      log_buffer_storage_current_reading = log_buffer_storage2;
    }
    log_buffer_storage_writing_head = 0;
  }

  // If there is data to read, start flushing
  if (log_buffer_size_to_read > 0) {
    atomic_set(&log_buffer_storage_read_lock, 1);
    EXIT_CRITICAL();

#if defined(_DMA_ENABLED) && defined(_UART_BACKEND_DMA)
    direct_dma_print((const uint8_t *)log_buffer_storage_current_reading,
                     log_buffer_size_to_read);
    // Note: read_lock is released by dma_tx_complete_callback
#else
    v_print((const char *)log_buffer_storage_current_reading);
    atomic_set(&log_buffer_storage_read_lock, 0);
#endif

    ENTER_CRITICAL();
    log_buffer_size_to_read = 0;
    EXIT_CRITICAL();
  } else {
    EXIT_CRITICAL();
  }
#endif
#endif
}

volatile uint32_t systick_count = 0;
#define ICSR (*(volatile uint32_t *)0xE000ED04)
#define ICSR_PENDSVSET (1 << 28) // Set this to trigger PendSV
#define ICSR_PENDSVCLR (1 << 27) // Clear PendSV
extern uint8_t scheduler_running;
void SysTick_Handler(void) {
  systick_count++;
  if ((systick_count % 10) == 0) {
    v_log_flush();
  }
  if ((scheduler_running == 123) && (systick_count % TIME_SLICE == 0))
    ICSR |= ICSR_PENDSVSET; // Trigger PENDSV
}

uint32_t v_get_ticks(void) { return systick_count; }

void *v_memset(void *s, int c, unsigned int n) {
  uint8_t *p = (uint8_t *)s;
  for (unsigned int i = 0; i < n; i++) {
    p[i] = (uint8_t)c;
  }
  return s;
}
void *v_memcpy(void *dest, const void *src, unsigned int n) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;

  // Handle trivial case
  if (dest == src || n == 0)
    return dest;

  // If dest < src, copy forward
  if (d < s) {
    for (unsigned int i = 0; i < n; i++)
      d[i] = s[i];
  } else {
    // Overlapping regions — copy backwards
    for (unsigned int i = n; i != 0; i--)
      d[i - 1] = s[i - 1];
  }

  return dest;
}
uint32_t v_strlen(const char *s) {
  if (!s)
    return 0;
  uint32_t count = 0;
  while (s[count] != '\0' && count != ~(0)) {
    count++;
  }
  return count;
}