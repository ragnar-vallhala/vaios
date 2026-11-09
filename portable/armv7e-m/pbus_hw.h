#ifndef VAIOS_PBUS_HW_H
#define VAIOS_PBUS_HW_H

// I2C and SPI bus managers over NavHAL (the port side of include/periph_bus.h): a v_pbus_t (periph_bus.h) bound to one
// peripheral, plus the devices on it. Tasks reach a device by name through
// v_pbus_open / v_pbus_xfer / v_pdev_read_reg; they never see the bus.
//
//   static v_pbus_mgr_t i2c1;
//   v_pbus_i2c_attach(&i2c1, HAL_I2C_1, &cfg, GPIO_PB08, GPIO_PB09, HAL_GPIO_AF4);
//   v_pbus_i2c_add(&i2c1, "baro0", 0x77);
//   task_create_privileged(i2c1_worker, &i2c1, 1024, 6, "i2c1");
//   static void i2c1_worker(void *m) { for (;;) v_pbus_mgr_step(m, 100); }
//
// Every hardware call starts in the worker, a privileged task above the
// bus's users: NavHAL's I2C/SPI calls spin, and a spin inside a syscall or
// an ISR would stall everything below it. The worker runs polled transfers to
// the end; an I2C register read (one register byte out, then a read) goes out
// over DMA where NavHAL can (I2C1 on the F4: its DMA path is wired only
// there), and its completion IRQ finishes the job and starts the next.
//
// Each transfer has a deadline (`timeout` ticks). A DMA read past it, or a
// HAL error, runs the bus's recovery and fails the transfer with
// V_PBUS_ERESET: I2C clocks a stuck slave off SDA (9 SCL pulses + STOP) and
// re-initialises the peripheral; SPI raises CS and re-initialises.
//
// Privileged, at boot, except v_pbus_mgr_step (the worker task).

#include "periph_bus.h"
#include <stdint.h>

#if defined(NAVHAL) && VAIOS_DEVFS
#include "navhal.h"

#define V_PBUS_EIO (-5)      // the HAL reported an error (NACK, overrun ...)
#define V_PBUS_ERESET (-104) // the bus was recovered; the transfer was lost

typedef struct v_pbus_mgr v_pbus_mgr_t;

// One SPI device: its chip select and bus settings, applied whenever the bus
// switches to it. CS is driven by the manager only.
typedef struct {
  v_pbus_mgr_t *m;
  hal_gpio_pin_t cs;
  hal_spi_config_t cfg;
  uint32_t hz;
} v_spi_dev_t;

struct v_pbus_mgr {
  v_pbus_t bus;
  uint32_t timeout;      // ticks a transfer may take, start to completion
  uint32_t errors;       // transfers failed by the HAL or the deadline
  uint32_t recoveries;   // bus resets
  // --- private ---
  uint8_t kind;
  uint8_t hw;            // hal_i2c_bus_t / hal_spi_instance_t
  uint8_t dma_ok;        // I2C: register reads may use DMA
  volatile uint8_t dma;  // a DMA read is in flight
  hal_i2c_config_t i2c_cfg;
  hal_gpio_pin_t scl, sda;
  hal_gpio_af_t af;
  const v_spi_dev_t *spi_cur; // settings the SPI peripheral holds now
  const v_spi_dev_t *dev;     // the pending transfer's SPI device
  const v_pbus_xfer_t *x;     // the pending transfer, NULL if none
  volatile uint32_t t0;       // when it started
  StaticSemaphore_t kick_buf __attribute__((aligned(8)));
  SemaphoreHandle_t kick;
};

#if NAVHAL_CONFIG_DRV_I2C
// Bind `m` to an I2C instance and initialise it with `cfg`. scl/sda/af are the
// bus's pins and alternate function: recovery bit-bangs them, then restores.
int v_pbus_i2c_attach(v_pbus_mgr_t *m, hal_i2c_bus_t hw,
                      const hal_i2c_config_t *cfg, hal_gpio_pin_t scl,
                      hal_gpio_pin_t sda, hal_gpio_af_t af);
// A device at 7-bit `addr` on the bus, opened by tasks as `name`.
int v_pbus_i2c_add(v_pbus_mgr_t *m, const char *name, uint8_t addr);
#endif

#if NAVHAL_CONFIG_DRV_SPI
// Bind `m` to an SPI instance. Its settings come from each device.
int v_pbus_spi_attach(v_pbus_mgr_t *m, hal_spi_instance_t hw);
// Register `d` (fill cs/cfg/hz first; it must outlive the registration) as
// `name`. Sets its CS pin up as an output, high.
int v_pbus_spi_add(v_pbus_mgr_t *m, v_spi_dev_t *d, const char *name);
#endif

#if NAVHAL_CONFIG_DRV_I2C || NAVHAL_CONFIG_DRV_SPI
// The worker: wait up to `ticks` for a transfer, run it, and enforce the
// deadline of a DMA one in flight. Loop on it in a privileged task.
void v_pbus_mgr_step(v_pbus_mgr_t *m, uint32_t ticks);
#endif

#endif // NAVHAL && VAIOS_DEVFS
#endif
