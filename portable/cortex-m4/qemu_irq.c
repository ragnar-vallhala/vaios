#include "qemu_irq.h"
#include <stddef.h>

#define MAX_IRQ 64

static void (*irq_callbacks[MAX_IRQ])(void);

void qemu_irq_init(void)
{
    for (int i = 0; i < MAX_IRQ; i++)
        irq_callbacks[i] = NULL;
}

void hal_interrupt_attach_callback(int irq, void (*cb)(void))
{
    irq_callbacks[irq] = cb;
}

void hal_enable_interrupt(int irq)
{
    // Nothing needed for QEMU
}

void qemu_trigger_irq(int irq)
{
    if (irq_callbacks[irq])
        irq_callbacks[irq]();
}

