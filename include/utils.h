#ifndef VAIOS_UTILS_H
#define VAIOS_UTILS_H
#include "vaios_config.h"
#include <stdint.h>
void print(const char *str);
void print_fmt(const char *fmt, ...);
int print_fmt_buf(char *out, uint32_t out_size, const char *fmt, ...);

uint32_t v_get_ticks(void);

typedef enum {
  LOG_TRACE, // Extremely fine-grained information (every function call,
             // variable value changes)
  LOG_DEBUG, // Useful for developers to see what’s happening internally
  LOG_INFO,  // High-level flow information, job completions, status update, etc
  LOG_WARN,  // Something unexpected happened, but the system can still recover
  LOG_ERROR, // A failure occurred, but the system can still run in degraded
             // mode
  LOG_FATAL  // A severe error that prevents the system from continuing
} Log_Type;

// ANSI color codes
#define COLOR_RESET "\x1B[0m"
#define COLOR_TRACE "\x1B[37m"   // White
#define COLOR_DEBUG "\x1B[36m"   // Cyan
#define COLOR_INFO "\x1B[32m"    // Green
#define COLOR_WARN "\x1B[33m"    // Yellow
#define COLOR_ERROR "\x1B[31m"   // Red
#define COLOR_FATAL "\x1B[35m"   // Magenta
#define COLOR_UNKNOWN "\x1B[34m" // Blue
void v_log(Log_Type type, const char *msg, ...);
void v_log_flush(void);
void dma_tx_complete_callback(void);
// Useful Functions

void *v_memset(void *s, int c, unsigned int n);
void *v_memcpy(void *dest, const void *src, unsigned int n);
uint32_t v_strlen(const char *s);
float v_atof(const char *s);

extern volatile uint8_t is_panicking;
void v_panic(const char *file, int line, const char *fmt, ...);

// Use safely only if DMA is enabled and you know what you are doing
void direct_dma_print(const uint8_t *bytes, uint32_t len);

int v_strcmp(const char *s1, const char *s2);
int v_strncmp(const char *s1, const char *s2, int n);
typedef struct {
  Log_Type type;
  char msg[LOG_MSG_MAX_LEN];
} LogEntry;

#endif //! VAIOS_UTILS_H
