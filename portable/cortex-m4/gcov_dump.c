/**
 * @file gcov_dump.c
 * @brief On-target gcov (.gcda) dumper for bare-metal coverage under Renode.
 *
 * Host gcov cannot measure the ARM-only enforcement code (context switch, MPU
 * regions, SVC dispatch, fault handlers, semihosting) because those files are
 * never compiled on the host. This dumper closes that gap: build the on-target
 * unit-test image with -DVAIOS_GCOV=ON (adds --coverage), run it under Renode,
 * and at test end it emits every instrumented TU's .gcda profile as base64 over
 * the UART console. tools/gcov_uart_decode.py reconstructs the real .gcda files
 * on the host — right next to the .gcno the compiler emitted — so `gcov` reports
 * line/branch coverage of the ARM sources. See tools/coverage_target.sh.
 *
 * Why over UART and not semihosting? The arm-none-eabi libgcov is the
 * freestanding variant with NO file I/O (its only external symbol is strlen), so
 * .gcda must be serialised by us via __gcov_info_to_gcda(). The natural sink
 * would be a semihosting host file, but Renode 1.16.1 does not implement ARM
 * semihosting, and the NAVHAL enforcement code (port_hw.c is wholly #ifdef
 * NAVHAL) only runs under Renode. The UART is the one host-visible channel Renode
 * models, and the suite already streams its log over it, so we reuse it — framed
 * and base64-encoded so binary .gcda survives the text channel intact.
 *
 * Getting the gcov_info list: the image is built with plain --coverage (NOT
 * -fprofile-info-section, which nulls the embedded filename), so each TU emits a
 * static constructor that calls __gcov_init(info) with the real .gcda path
 * inside `info`. We override __gcov_init to collect those pointers, stub the
 * __gcov_exit the destructors reference, and run the constructors ourselves from
 * .init_array — the vaios/NavHAL startup does not run them. tools/gcov_sections.ld
 * (merged via -T) bounds .init_array with __init_array_start/end without editing
 * NavHAL's linker script.
 *
 * The whole file compiles to nothing unless VAIOS_GCOV is defined, so it is inert
 * in every normal build even though the portable glob always includes it.
 */
#if defined(VAIOS_GCOV)

#include "gcov_dump.h"
#include <stddef.h>
#include <stdint.h>

/* UART console (polling path via the port facade — safe under Renode). */
extern void v_print(const char *s);

/* Opaque to us; __gcov_info_to_gcda knows the real layout. */
struct gcov_info;

extern void __gcov_info_to_gcda(const struct gcov_info *info,
                                void (*filename_fn)(const char *, void *),
                                void (*dump_fn)(const void *, unsigned, void *),
                                void *(*allocate_fn)(unsigned, void *),
                                void *arg);

/* .init_array bounds provided by tools/gcov_sections.ld (INSERT AFTER .text). */
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);

/* Frame markers. Chosen so they can never appear inside a base64 payload (the
 * base64 alphabet excludes '@'); the host decoder keys off these exactly. */
#define GCDA_BEGIN "@@VAIOS_GCDA_BEGIN "
#define GCDA_END "@@VAIOS_GCDA_END"

/* --------------------------------------------------------------------------
 * gcov_info registry. The compiler's per-TU constructors call __gcov_init; we
 * override libgcov's version so they register into our own table instead of the
 * driver's internal list (which this freestanding libgcov never dumps).
 * -------------------------------------------------------------------------- */
#ifndef VAIOS_GCOV_MAX_INFOS
#define VAIOS_GCOV_MAX_INFOS 64
#endif
static const struct gcov_info *g_infos[VAIOS_GCOV_MAX_INFOS];
static int g_n_infos;

void __gcov_init(struct gcov_info *info) {
  if (info && g_n_infos < VAIOS_GCOV_MAX_INFOS)
    g_infos[g_n_infos++] = info;
}

/* The instrumented TUs' destructors reference __gcov_exit (the driver's file
 * writer, absent in this libgcov). We never run destructors; a stub satisfies
 * the link. */
void __gcov_exit(void) {}

/* libgcov's dump_string needs strlen, which -nostdlib omits. Weak so libc's
 * wins if the image also links one; resolves the reference otherwise. */
__attribute__((weak)) size_t strlen(const char *s) {
  const char *p = s;
  while (*p)
    ++p;
  return (size_t)(p - s);
}

/* --------------------------------------------------------------------------
 * base64 line encoder over the UART. dump_fn feeds raw .gcda bytes here; they
 * are encoded in 3-byte groups and flushed as fixed-width lines so Renode's
 * per-line UART logging (and the host decoder) stay in sync.
 * -------------------------------------------------------------------------- */
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

#define B64_LINE_GROUPS 15 /* 15 groups -> 60 base64 chars per line */

typedef struct {
  uint8_t in[3];
  int nin;      /* bytes pending in `in` (0..2) */
  char line[4 * B64_LINE_GROUPS + 2];
  int nline;    /* base64 chars buffered in `line` */
} b64_t;
static b64_t g_b64;

static void b64_flush_line(b64_t *e) {
  if (e->nline == 0)
    return;
  e->line[e->nline] = '\0';
  v_print(e->line);
  v_print("\r\n");
  e->nline = 0;
}

static void b64_emit_group(b64_t *e) {
  uint32_t v = ((uint32_t)e->in[0] << 16) | ((uint32_t)e->in[1] << 8) | e->in[2];
  e->line[e->nline++] = B64[(v >> 18) & 0x3F];
  e->line[e->nline++] = B64[(v >> 12) & 0x3F];
  e->line[e->nline++] = (e->nin > 1) ? B64[(v >> 6) & 0x3F] : '=';
  e->line[e->nline++] = (e->nin > 2) ? B64[v & 0x3F] : '=';
  if (e->nline >= 4 * B64_LINE_GROUPS)
    b64_flush_line(e);
}

static void b64_reset(b64_t *e) {
  e->nin = 0;
  e->nline = 0;
}

static void b64_byte(b64_t *e, uint8_t b) {
  e->in[e->nin++] = b;
  if (e->nin == 3) {
    b64_emit_group(e);
    e->nin = 0;
  }
}

static void b64_finish(b64_t *e) {
  if (e->nin > 0) {
    /* pad the final partial group */
    if (e->nin == 1)
      e->in[1] = 0;
    if (e->nin <= 2)
      e->in[2] = 0;
    b64_emit_group(e);
    e->nin = 0;
  }
  b64_flush_line(e);
}

/* --------------------------------------------------------------------------
 * __gcov_info_to_gcda callbacks. One framed base64 stream per gcov_info.
 * -------------------------------------------------------------------------- */
/* Scratch buffer __gcov_info_to_gcda allocates counter records into. Its
 * content is streamed out by dump_fn before the next allocation, so a bump arena
 * reset once per file is safe; sized well above any single kernel TU's needs.
 * Overflow is loud rather than silent. */
#ifndef VAIOS_GCOV_ARENA
#define VAIOS_GCOV_ARENA 8192
#endif
static unsigned char g_arena[VAIOS_GCOV_ARENA];
static unsigned g_arena_off;

static void gcda_filename(const char *filename, void *arg) {
  (void)arg;
  g_arena_off = 0; /* reset scratch for this file */
  b64_reset(&g_b64);
  v_print(GCDA_BEGIN);
  v_print(filename ? filename : "(null)");
  v_print("\r\n");
}

static void gcda_dump(const void *data, unsigned length, void *arg) {
  (void)arg;
  const uint8_t *p = (const uint8_t *)data;
  for (unsigned i = 0; i < length; ++i)
    b64_byte(&g_b64, p[i]);
}

static void *gcda_allocate(unsigned length, void *arg) {
  (void)arg;
  if (g_arena_off + length > sizeof(g_arena)) {
    v_print("@@VAIOS_GCDA_ERROR arena overflow — raise VAIOS_GCOV_ARENA\r\n");
    return NULL;
  }
  void *p = &g_arena[g_arena_off];
  g_arena_off += length;
  return p;
}

void vaios_gcov_dump(void) {
  /* Run the per-TU constructors so every gcov_info registers (counters have
   * been accumulating independently since boot; this only builds the list). */
  for (void (**ctor)(void) = __init_array_start; ctor < __init_array_end; ++ctor)
    (*ctor)();

  v_print("@@VAIOS_GCDA_DUMP_BEGIN\r\n");
  for (int i = 0; i < g_n_infos; ++i) {
    __gcov_info_to_gcda(g_infos[i], gcda_filename, gcda_dump, gcda_allocate,
                        NULL);
    b64_finish(&g_b64);
    v_print(GCDA_END "\r\n");
  }
  v_print("@@VAIOS_GCDA_DUMP_END\r\n");
}

#endif /* VAIOS_GCOV */
