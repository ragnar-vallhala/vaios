#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>

/*
 * Phase-2 MPU positive tests — one probe per boot (each faults and halts).
 * Select the probe with -DMPU_FAULT_MODE=N (see examples/CMakeLists.txt):
 *   0 = W^X:        execute an instruction from SRAM     -> IACCVIOL (MMFSR 0x1)
 *   1 = NULL guard: write through a NULL pointer         -> DACCVIOL (MMFSR 0x82, addr 0x0)
 *   2 = flash-RO:   write into the flash code region     -> DACCVIOL (MMFSR 0x82)
 * Each returns cleanly (and warns) if its region is not armed, so the same
 * image is a pass/fail probe either way.
 */
#ifndef MPU_FAULT_MODE
#define MPU_FAULT_MODE 0
#endif

#if MPU_FAULT_MODE == 0
static volatile uint16_t ram_code[4]; /* lands in .bss = SRAM */
#endif

void mpu_fault_task(void *args) {
  (void)args;
#if MPU_FAULT_MODE == 0
  v_log(LOG_INFO, "W^X test (task %d): assembling BX LR into SRAM @ %p",
        GET_CURRENT_TASK_ID(), (void *)ram_code);
  ram_code[0] = 0x4770u; /* Thumb: BX LR */
  __asm volatile("dsb" ::: "memory");
  __asm volatile("isb" ::: "memory");
  void (*ram_fn)(void) = (void (*)(void))((uintptr_t)ram_code | 1u);
  v_log(LOG_INFO, "Jumping to RAM -> expect MPU fault (IACCVIOL, MMFSR 0x1)");
  ram_fn(); /* execute-from-RAM: W^X should fault on this instruction fetch */
  v_log(LOG_WARN, "RETURNED with NO fault -- W^X not active!");
#elif MPU_FAULT_MODE == 1
  v_log(LOG_INFO, "NULL-guard test (task %d): booted OK -> VTOR relocated",
        GET_CURRENT_TASK_ID());
  v_delay(20);
  v_log(LOG_INFO, "Write through NULL -> expect MPU fault (DACCVIOL, MMFSR 0x82)");
  *(volatile uint32_t *)0u = 0xDEADBEEFu; /* NULL deref -> no-access region */
  v_log(LOG_WARN, "NULL write did NOT fault -- NULL guard not active!");
#elif MPU_FAULT_MODE == 2
  v_log(LOG_INFO, "flash-RO test (task %d): writing into the flash code region",
        GET_CURRENT_TASK_ID());
  v_delay(20);
  v_log(LOG_INFO, "Write to 0x08000100 -> expect MPU fault (DACCVIOL, MMFSR 0x82)");
  *(volatile uint32_t *)0x08000100u = 0xDEADBEEFu; /* write into RO flash region */
  v_log(LOG_WARN, "flash write did NOT fault -- flash-RO not active!");
#endif
  while (1)
    v_delay(1000);
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);

  v_log(LOG_INFO, "Starting MPU_FAULT probe (mode %d)", MPU_FAULT_MODE);
  task_create(mpu_fault_task, NULL, 2048, 1);
  scheduler_start();

  while (1)
    ;
}
