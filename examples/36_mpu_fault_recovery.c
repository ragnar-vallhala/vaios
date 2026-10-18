#include "navhal.h"
#include "port.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include "vfile.h"
#include <stdint.h>

/*
 * MPU fault recovery, on-target (VAIOS_MPU_FAULT_RECOVERY, default policy
 * RESTART with a cap of 3), one task per fault policy.
 *
 * Unprivileged tasks that fault on every run, each a different way:
 *   - escaper reads kernel SRAM (a data-access violation, frame stacked fine);
 *   - overflow recurses off the bottom of its stack, so PSP is left where the
 *     task may not write (the switch-out must not push there). On the F401 the
 *     guard stops it, with MSTKERR: the frame itself could not be stacked.
 *     Renode lets it run through the guard (its MPU model does not give the
 *     higher-numbered region the overlap) to the end of the block region --
 *     contained either way.
 *   - child, spawned by survivor with RESTART and a cap of 1, reads kernel
 *     SRAM too.
 * Policies: escaper keeps the default (restart, cap 3: runs 4 times), overflow
 * is set to KILL by main (runs once), child runs twice. So faults = 4 + 1 + 2
 * = 7 and restarts = 3 + 0 + 1 = 4. survivor also asks for a PANIC child,
 * which an unprivileged parent may not have, then ticks throughout and must
 * still be ticking after a privileged monitor has read the kernel's counters.
 *
 * With VAIOS_MPU_FAULT_PRIV, a PRIVILEGED task (priv, RESTART with a cap of 1)
 * jumps into SRAM, which is execute-never for privileged code too: it runs
 * twice, so faults = 9 and restarts = 5.
 *
 * With VAIOS_MPU_FAULT_ISR, a privileged task (irqtest) makes the USART1 and
 * USART6 IRQs (neither is a Nucleo console) containable and hooks a handler
 * into both that jumps into SRAM. It pends USART1 with no FP state live (a
 * basic frame), then USART6 with s0 live (an extended frame, lazily stacked),
 * then USART1 again: two faults contained, both IRQs disabled, so the third
 * pend runs nothing; irqtest carries on each time, s0 intact.
 *
 * Built with -DFR_PANIC=1, main gives escaper the PANIC policy instead: its
 * first fault must stop the system. -DFR_PRIV_CRIT=1 makes priv fault inside a
 * critical section, which must stop the system whatever its policy.
 * -DFR_ISR_PANIC=1 leaves irqtest's IRQs not containable: the first must stop
 * the system. -DFR_NO_OVERFLOW=1 leaves overflow out
 * (6 faults, 4 restarts), for a simulator that does not model the MPU's
 * subregion-disable bits.
 */

extern void v_print(const char *s); /* polling console write (privileged) */

/* With VAIOS_MPU_STACK_SUBREGIONS the stacks are NOT powers of two, so every
 * block sits in the top eighths of its region above disabled subregions, and
 * the same faults have to be caught there. Without it they must be. */
#if VAIOS_MPU_STACK_SUBREGIONS
#define STK_BIG 1536u  /* 6/8 of a 2 KiB region */
#define STK_SMALL 768u /* 6/8 of a 1 KiB region */
#else
#define STK_BIG 2048u
#define STK_SMALL 1024u
#endif

static void uputs(const char *s) {
  int n = 0;
  while (s[n])
    n++;
  v_file_write(1, s, n); /* syscall: unprivileged-safe */
}

static void escaper(void *arg) {
  (void)arg;
  uputs("[fr] escaper start\r\n");
  volatile uint32_t v = *(volatile uint32_t *)0x20000010; /* kernel SRAM */
  (void)v;
  uputs("[fr] BUG: escaper not stopped\r\n");
  for (;;)
    v_delay(1000);
}

static int dive(int depth) {
  volatile uint8_t pad[8]; /* small frames, so the stores walk into the guard */
  pad[0] = (uint8_t)depth;
  if (depth > 1000000) /* never: the guard stops it far sooner */
    return 0;
  return dive(depth + 1) + pad[0]; /* not a tail call */
}

static void overflow(void *arg) {
  (void)arg;
  uputs("[fr] overflow start\r\n");
  dive(0);
  uputs("[fr] BUG: overflow not stopped\r\n");
  for (;;)
    v_delay(1000);
}

static void child(void *arg) {
  (void)arg;
  uputs("[fr] child start\r\n");
  volatile uint32_t v = *(volatile uint32_t *)0x20000010; /* kernel SRAM */
  (void)v;
  uputs("[fr] BUG: child not stopped\r\n");
  for (;;)
    v_delay(1000);
}

#if VAIOS_MPU_FAULT_PRIV
static void priv(void *arg) {
  (void)arg;
  v_print("[fr] priv start\r\n");
#ifdef FR_PRIV_CRIT
  ENTER_CRITICAL();
#endif
  uint16_t ram[2] = {0x4770u, 0x4770u}; /* bx lr: never runs, SRAM is XN */
  ((void (*)(void))((uintptr_t)ram | 1u))();
  v_print("[fr] BUG: priv not stopped\r\n");
  for (;;)
    task_delay(1000);
}
#endif

#if VAIOS_MPU_FAULT_ISR
#define NVIC_ISPR ((volatile uint32_t *)0xE000E200)
static void bad_isr(void) {
  uint16_t ram[2] = {0x4770u, 0x4770u}; /* bx lr: never runs, SRAM is XN */
  ((void (*)(void))((uintptr_t)ram | 1u))();
}

static void pend(int irq) {
  NVIC_ISPR[irq >> 5] = 1u << (irq & 31); /* the IRQ fires right here */
  __asm volatile("dsb\n isb" ::: "memory");
}

static void irqtest(void *arg) {
  (void)arg;
  task_delay(100);
  const int irqs[2] = {USART1_IRQn, USART6_IRQn};
  for (int i = 0; i < 2; i++) {
#ifndef FR_ISR_PANIC /* not opted in: the first fault must stop the system */
    v_irq_set_containable(irqs[i]);
#endif
    hal_interrupt_attach_callback(irqs[i], bad_isr);
    hal_interrupt_enable_with_priority(irqs[i], HAL_IRQ_PRIORITY_DEFAULT);
  }
  pend(irqs[0]);
  v_print("[fr] irqtest resumed\r\n");
#ifdef _FPU_ENABLED
  /* s0 live across the IRQ: the task's frame is the extended one. */
  uint32_t in = 0x40490FDBu, out = 0;
  __asm volatile("vmov s0, %[in]      \n"
                 "str %[bit], [%[ispr]] \n"
                 "dsb                 \n"
                 "isb                 \n"
                 "vmov %[out], s0     \n"
                 : [out] "=&r"(out)
                 : [in] "r"(in), [bit] "r"(1u << (irqs[1] & 31)),
                   [ispr] "r"(&NVIC_ISPR[irqs[1] >> 5])
                 : "s0", "memory");
  v_print(out == in ? "[fr] irqtest resumed, s0 kept\r\n"
                    : "[fr] BUG: s0 lost\r\n");
#else
  pend(irqs[1]);
  v_print("[fr] irqtest resumed, s0 kept\r\n");
#endif
  pend(irqs[0]); /* disabled now: nothing runs */
  v_print("[fr] irqtest resumed\r\n");
  int a = v_irq_failed_take(), b = v_irq_failed_take(), c = v_irq_failed_take();
  char buf[80];
  print_fmt_buf(buf, sizeof buf, "[fr] irq failed=%d,%d,%d expected=%d,%d,-1\r\n",
                a, b, c, irqs[0], irqs[1]);
  v_print(buf);
  for (;;)
    task_delay(1000);
}
#endif

static void survivor(void *arg) {
  (void)arg;
  v_task_spawn_t cfg = {.entry = child, .stack_size = STK_SMALL, .priority = 1,
                        .name = "child", .fault_policy = V_FAULT_PANIC};
  uputs(v_task_spawn(&cfg) == V_TASK_EPERM ? "[fr] panic child refused\r\n"
                                           : "[fr] BUG: panic child allowed\r\n");
  cfg.fault_policy = V_FAULT_RESTART;
  cfg.fault_restart_max = 1;
  if (v_task_spawn(&cfg) <= 0)
    uputs("[fr] BUG: child spawn failed\r\n");
  for (;;) {
    uputs("[fr] survivor tick\r\n");
    v_delay(250);
  }
}

static void monitor(void *arg) {
  (void)arg;
  task_delay(1500); /* every restart is long done by now */
  /* Repeated, so a USB CDC console attached after boot still sees it. */
  for (;;) {
    char buf[112];
    print_fmt_buf(buf, sizeof buf,
                  "[fr] faults=%u restarts=%u max=%u | last: task %u addr 0x%x "
                  "MMFSR 0x%x\r\n",
                  (unsigned)v_mpu_fault_stats.faults,
                  (unsigned)v_mpu_fault_stats.restarts,
                  (unsigned)VAIOS_MPU_FAULT_RESTART_MAX,
                  (unsigned)v_mpu_fault_stats.last_task,
                  (unsigned)v_mpu_fault_stats.last_addr,
                  (unsigned)v_mpu_fault_stats.last_mmfsr);
    v_print(buf);
#if VAIOS_MPU_FAULT_ISR
    print_fmt_buf(buf, sizeof buf, "[fr] isr_faults=%u last_irq=%u\r\n",
                  (unsigned)v_mpu_fault_stats.isr_faults,
                  (unsigned)v_mpu_fault_stats.last_irq);
    v_print(buf);
#endif
    v_print("[fr] DONE\r\n");
    task_delay(2000);
  }
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);
  v_print("[fr] mpu_fault_recovery: start\r\n");
  uint32_t esc = task_create_named(escaper, NULL, STK_BIG, 2, "escaper");
#ifdef FR_PANIC
  task_set_fault_policy(esc, V_FAULT_PANIC, 0);
#else
  (void)esc;
#endif
#ifndef FR_NO_OVERFLOW /* see tools/renode_fault_recovery.sh */
  uint32_t ov = task_create_named(overflow, NULL, STK_BIG, 2, "overflow");
  task_set_fault_policy(ov, V_FAULT_KILL, 0);
#endif
#if VAIOS_MPU_FAULT_PRIV
  uint32_t pv = task_create_privileged(priv, NULL, STK_BIG, 2, "priv");
  task_set_fault_policy(pv, V_FAULT_RESTART, 1);
#endif
#if VAIOS_MPU_FAULT_ISR
  task_create_privileged(irqtest, NULL, STK_BIG, 2, "irqtest");
#endif
  task_create_named(survivor, NULL, STK_SMALL, 1, "survivor");
  task_create_privileged(monitor, NULL, STK_BIG, 3, "monitor");
  scheduler_start();
  for (;;)
    ;
}
