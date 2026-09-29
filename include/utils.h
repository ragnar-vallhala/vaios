#ifndef VAIOS_UTILS_H
#define VAIOS_UTILS_H
#include "vaios_config.h"
#include <stdarg.h>
#include <stdint.h>
#include "stddef.h"
void print(const char *str);
void print_fmt(const char *fmt, ...);
int print_fmt_buf(char *out, uint32_t out_size, const char *fmt, ...);
int vaprint_fmt_buf(char *out, size_t out_size, const char *fmt, va_list args);

// Kernel-side direct log (printk): the privileged console path. Separate from
// the task-facing v_printf(), which formats and traps through write(fd 1).
#define printk print_fmt

#if VAIOS_DEVFS
// Task-facing printf: formats, then routes through write(1) -> /dev/console.
int v_printf(const char *fmt, ...);
// Kernel log ring backing /dev/kmsg (appended by the printk path; drained by a
// task read()ing /dev/kmsg).
void v_kmsg_append(const char *buf, uint32_t len);
int v_kmsg_read(char *out, uint32_t len);
#endif

uint32_t v_get_ticks(void);

// Kernel SysTick body: advance the tick, wake delayed tasks, pend PendSV. The
// arch vector handler (portable/<arch>/) calls this, so the CMSIS vector name
// stays out of the kernel.
void v_kernel_tick(void);

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

// Kernel-internal log macro. Calls in kernel hot paths (memory, task, ipc)
// route through this so the entire call site can be compiled out below a
// chosen severity. Avoids paying the v_log() prologue cost (PSP read,
// stack-overflow check, module_allowed scan, vararg promotion) when the
// message would just be filtered.
#ifndef VAIOS_KERNEL_LOG_LEVEL
#define VAIOS_KERNEL_LOG_LEVEL LOG_WARN
#endif
#define V_KLOG(level, ...)                                                     \
  do {                                                                         \
    if ((int)(level) >= (int)(VAIOS_KERNEL_LOG_LEVEL))                         \
      v_log((level), __VA_ARGS__);                                             \
  } while (0)
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
