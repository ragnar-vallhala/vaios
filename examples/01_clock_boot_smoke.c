/**
 * @file clock_boot_smoke.c
 * @brief Clock bring-up smoke test (NAVHAL path).
 *
 * Calls v_init with internal_clock_setup = 1, which runs NavHAL's PLL bring-up.
 * If the clock's ready-bit waits are unbounded (the bug), this hangs forever on
 * a target whose PLL/HSE never reports ready — e.g. under QEMU, which doesn't
 * model the STM32F4 PLL. With NavHAL's bounded clock waits it times out, falls
 * back to the reset HSI, and v_init returns — so the PASS line prints.
 *
 * Used by tools/test_qemu_smoke.sh (clock case). Build NAVHAL=ON soft-float so
 * it runs on QEMU's FPU-less cortex-m4 (the FPU mirror picks soft from the
 * soft-float NavHAL config).
 */
#include "vaios.h"
#include "semihosting.h"

int main(void) {
    sh_write0("[CLOCK] calling v_init (internal_clock_setup=1)\n");
    vaios_init_config_t cfg = {.internal_clock_setup = 1, .internal_sd_card_setup = 0};
    v_init(&cfg);
    sh_write0("[CLOCK] PASS: v_init returned (clock bring-up did not hang)\n");
    sh_exit(0);
    return 0;
}
