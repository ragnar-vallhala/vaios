#ifndef VAIOS_GCOV_DUMP_H
#define VAIOS_GCOV_DUMP_H

/*
 * On-target gcov (.gcda) dump entry point. Implemented in
 * portable/cortex-m4/gcov_dump.c, and only when the image is built with
 * -DVAIOS_GCOV=ON (--coverage instrumentation + tools/gcov_sections.ld).
 *
 * Renode 1.16.1 does not implement ARM semihosting, and the NAVHAL enforcement
 * code (port_hw.c is entirely #ifdef NAVHAL) only runs correctly under Renode —
 * so .gcda cannot be written to the host over semihosting. Instead each .gcda is
 * serialised with __gcov_info_to_gcda and streamed out as base64 over the same
 * UART console the test log uses, framed with markers. tools/gcov_uart_decode.py
 * reconstructs the real .gcda files on the host from the captured UART log, next
 * to the .gcno the compiler emitted. Drive the whole flow via
 * tools/coverage_target.sh.
 *
 * Call once at the end of an on-target run.
 */

#ifdef __cplusplus
extern "C" {
#endif

void vaios_gcov_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* VAIOS_GCOV_DUMP_H */
