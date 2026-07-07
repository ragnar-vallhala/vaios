#include "task.h"
#include "utils.h"
#include "vaios.h"
#include <stdint.h>

/*
 * Phase-2 W^X positive test: prove the static SRAM execute-never region traps
 * an instruction fetch from RAM.
 *
 * The task assembles a one-instruction Thumb routine — `BX LR` (0x4770), which
 * just returns — into an SRAM buffer at runtime, then calls it. With
 * VAIOS_MPU_STATIC_PROTECT on, the SRAM region is execute-never, so the
 * instruction fetch from RAM must trap to MemManage_Handler as an
 * instruction-access violation (MMFSR IACCVIOL, bit 0 -> "MMFSR 0x1"). Without
 * W^X the call returns cleanly and the task prints the "NOT active" warning —
 * so the same image is a pass/fail probe either way.
 */
static volatile uint16_t ram_code[4]; /* lands in .bss = SRAM */

void mpu_fault_task(void *args) {
  (void)args;
  v_log(LOG_INFO, "MPU W^X test (task %d): assembling BX LR into SRAM @ %p",
        GET_CURRENT_TASK_ID(), (void *)ram_code);

  ram_code[0] = 0x4770u; /* Thumb: BX LR */
  __asm volatile("dsb" ::: "memory");
  __asm volatile("isb" ::: "memory");

  /* OR in the Thumb bit so the call switches to Thumb state at the RAM target. */
  void (*ram_fn)(void) = (void (*)(void))((uintptr_t)ram_code | 1u);

  v_log(LOG_INFO, "Jumping to RAM now — expect MPU fault (MMFSR IACCVIOL=0x1)");
  ram_fn(); /* <-- execute-from-RAM: W^X should fault on this instruction fetch */

  v_log(LOG_WARN, "RETURNED from RAM with NO fault — W^X is NOT active!");
  while (1)
    v_delay(1000);
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);

  v_log(LOG_INFO, "Starting MPU W^X (execute-from-RAM) fault test");
  task_create(mpu_fault_task, NULL, 2048, 1);
  scheduler_start();

  while (1)
    ;
}
