// I2C / SPI bus managers over NavHAL -- see include/pbus_hw.h.
#include "pbus_hw.h"

#if defined(NAVHAL) && VAIOS_DEVFS &&                                          \
    (NAVHAL_CONFIG_DRV_I2C || NAVHAL_CONFIG_DRV_SPI)
#include "port.h" // ENTER/EXIT_CRITICAL_FROM_ISR, v_port_trigger_pendsv
#include "task.h"
#include "utils.h" // v_get_ticks

enum { MGR_I2C = 1, MGR_SPI };

#define MGR_DEFAULT_TIMEOUT 20 // ticks

// v_pdev start hook, any context (the queue may dispatch from an ISR): hand
// the transfer to the worker. Never blocks.
static int mgr_start(v_pbus_mgr_t *m, const v_spi_dev_t *dev,
                     const v_pbus_xfer_t *x) {
  m->dev = dev;
  m->x = x;
  m->t0 = v_get_ticks();
  int woken = 0;
  v_semaphore_give_from_isr(m->kick, &woken);
  if (woken)
    v_port_trigger_pendsv();
  return 0;
}

// Finish the pending transfer and hand the bus to the next job.
static void finish(v_pbus_mgr_t *m, int rc) {
  if (rc < 0)
    m->errors++;
  m->x = NULL;
  v_pbus_done_isr(&m->bus, rc);
}

static int mgr_init(v_pbus_mgr_t *m, uint8_t kind, uint8_t hw) {
  if (!m || m->kick)
    return V_PBUS_EINVAL; // NULL, or attached twice
  m->kind = kind;
  m->hw = hw;
  if (!m->timeout)
    m->timeout = MGR_DEFAULT_TIMEOUT;
  m->kick = v_semaphore_create_binary_static(&m->kick_buf);
  return m->kick ? 0 : V_PBUS_EINVAL;
}

// --- I2C --------------------------------------------------------------------
#if NAVHAL_CONFIG_DRV_I2C
// ~5 us at 84 MHz: half an SCL period at 100 kHz. ponytail: a busy loop,
// scaled for 84 MHz; slower clocks only make the pulses longer, which I2C
// slaves accept.
static void half_bit(void) {
  for (volatile int i = 0; i < 100; i++)
    ;
}

// Clock a slave that holds SDA low out of its byte: up to 9 SCL pulses until
// SDA is released, then a STOP. Then the pins go back to the peripheral.
static void i2c_bus_clear(v_pbus_mgr_t *m) {
  hal_gpio_set_output_type(m->scl, HAL_GPIO_OTYPE_OPEN_DRAIN);
  hal_gpio_set_output_type(m->sda, HAL_GPIO_OTYPE_OPEN_DRAIN);
  hal_gpio_write(m->scl, HAL_GPIO_HIGH);
  hal_gpio_write(m->sda, HAL_GPIO_HIGH);
  hal_gpio_set_mode(m->scl, HAL_GPIO_MODE_OUTPUT, HAL_GPIO_PULL_UP);
  hal_gpio_set_mode(m->sda, HAL_GPIO_MODE_OUTPUT, HAL_GPIO_PULL_UP);
  for (int i = 0; i < 9 && hal_gpio_read(m->sda) == HAL_GPIO_LOW; i++) {
    hal_gpio_write(m->scl, HAL_GPIO_LOW);
    half_bit();
    hal_gpio_write(m->scl, HAL_GPIO_HIGH);
    half_bit();
  }
  // STOP: SDA rises while SCL is high.
  hal_gpio_write(m->scl, HAL_GPIO_LOW);
  half_bit();
  hal_gpio_write(m->sda, HAL_GPIO_LOW);
  half_bit();
  hal_gpio_write(m->scl, HAL_GPIO_HIGH);
  half_bit();
  hal_gpio_write(m->sda, HAL_GPIO_HIGH);
  half_bit();
  hal_gpio_set_alternate_function(m->scl, m->af);
  hal_gpio_set_alternate_function(m->sda, m->af);
  hal_gpio_set_mode(m->scl, HAL_GPIO_MODE_AF, HAL_GPIO_PULL_UP);
  hal_gpio_set_mode(m->sda, HAL_GPIO_MODE_AF, HAL_GPIO_PULL_UP);
}

static void i2c_recover(v_pbus_mgr_t *m) {
  m->recoveries++;
  hal_i2c_deinit((hal_i2c_bus_t)m->hw); // drops a stuck BUSY / half transfer
  i2c_bus_clear(m);
  hal_i2c_init((hal_i2c_bus_t)m->hw, &m->i2c_cfg);
}

#if NAVHAL_CONFIG_DRV_I2C_DMA
// NavHAL keeps one I2C DMA callback and one active config for all buses, so
// one manager owns the DMA read path: the first one attached to I2C1.
static v_pbus_mgr_t *i2c_dma_mgr;

// DMA-complete IRQ (NavHAL has already sent the STOP).
static void i2c_dma_done(void) {
  v_pbus_mgr_t *m = i2c_dma_mgr;
  uint32_t s = ENTER_CRITICAL_FROM_ISR();
  int mine = m && m->dma;
  if (mine)
    m->dma = 0;
  EXIT_CRITICAL_FROM_ISR(s);
  if (mine) // else late, after the deadline already failed it
    finish(m, (int)m->x->rx_len);
}

static void i2c_dma_stop(v_pbus_mgr_t *m) {
  hal_dma_binding_t b;
  if (hal_i2c_dma_get_binding((hal_i2c_bus_t)m->hw, false, &b) != HAL_OK)
    return;
  hal_dma_config_t c = {.controller = b.controller, .stream = b.stream,
                        .channel = b.channel};
  hal_dma_stop(&c);
  hal_dma_clear_flags(&c);
}
#endif

static void i2c_run(v_pbus_mgr_t *m, const v_pbus_xfer_t *x) {
  hal_i2c_bus_t hw = (hal_i2c_bus_t)m->hw;
  uint8_t a = (uint8_t)x->addr;
  hal_status_t st;
#if NAVHAL_CONFIG_DRV_I2C_DMA
  if (m->dma_ok && x->tx_len == 1 && x->rx_len) {
    m->t0 = v_get_ticks();
    m->dma = 1;
    st = hal_i2c_read_regs_dma(hw, a, ((const uint8_t *)x->tx)[0],
                               (uint8_t *)x->rx, (uint16_t)x->rx_len,
                               i2c_dma_done);
    if (st == HAL_OK)
      return; // i2c_dma_done, or the deadline, finishes it
    uint32_t s = ENTER_CRITICAL_FROM_ISR();
    m->dma = 0;
    EXIT_CRITICAL_FROM_ISR(s);
  } else
#endif
  if (!x->rx_len)
    st = hal_i2c_write(hw, a, (const uint8_t *)x->tx, (uint16_t)x->tx_len);
  else if (!x->tx_len)
    st = hal_i2c_read(hw, a, (uint8_t *)x->rx, (uint16_t)x->rx_len);
  else
    st = hal_i2c_write_read(hw, a, (const uint8_t *)x->tx,
                            (uint16_t)x->tx_len, (uint8_t *)x->rx,
                            (uint16_t)x->rx_len);
  if (st == HAL_OK) {
    finish(m, (int)(x->rx_len ? x->rx_len : x->tx_len));
    return;
  }
  // NavHAL's F4 driver reports a NACK as a timeout waiting for ADDR, so a
  // missing device and a wedged bus look alike: reset either way. A reset
  // on a healthy bus costs only the re-init.
  i2c_recover(m);
  finish(m, V_PBUS_ERESET);
}

int v_pbus_i2c_attach(v_pbus_mgr_t *m, hal_i2c_bus_t hw,
                      const hal_i2c_config_t *cfg, hal_gpio_pin_t scl,
                      hal_gpio_pin_t sda, hal_gpio_af_t af) {
  if (!cfg)
    return V_PBUS_EINVAL;
  int rc = mgr_init(m, MGR_I2C, (uint8_t)hw);
  if (rc)
    return rc;
  m->i2c_cfg = *cfg;
  m->scl = scl;
  m->sda = sda;
  m->af = af;
  hal_i2c_init(hw, cfg); // NOT_INITIALIZED = already up: fine
#if NAVHAL_CONFIG_DRV_I2C_DMA
  hal_dma_binding_t b;
  if (hw == HAL_I2C_1 && !i2c_dma_mgr &&
      hal_i2c_dma_get_binding(hw, false, &b) == HAL_OK &&
      v_dma_claim((uint8_t)(b.controller == HAL_DMA_CONTROLLER_1 ? 1 : 2),
                  (uint8_t)b.stream, m) == VA_PASS) {
    i2c_dma_mgr = m;
    m->dma_ok = 1;
  }
#endif
  return 0;
}

static int i2c_dev_start(void *ctx, const v_pbus_xfer_t *x) {
  return mgr_start((v_pbus_mgr_t *)ctx, NULL, x);
}

int v_pbus_i2c_add(v_pbus_mgr_t *m, const char *name, uint8_t addr) {
  if (!m || m->kind != MGR_I2C || addr > 0x7F)
    return V_PBUS_EINVAL;
  return v_pdev_register(name, &m->bus, i2c_dev_start, m, addr);
}
#endif // NAVHAL_CONFIG_DRV_I2C

// --- SPI --------------------------------------------------------------------
#if NAVHAL_CONFIG_DRV_SPI
#define SPI_TIMEOUT_MS 10

static void spi_run(v_pbus_mgr_t *m, const v_spi_dev_t *d,
                    const v_pbus_xfer_t *x) {
  hal_spi_instance_t hw = (hal_spi_instance_t)m->hw;
  hal_status_t st = HAL_OK;
  if (m->spi_cur != d) {
    st = hal_spi_init_hz(hw, &d->cfg, d->hz);
    m->spi_cur = st == HAL_OK ? d : NULL;
  }
  if (st == HAL_OK) {
    hal_gpio_write(d->cs, HAL_GPIO_LOW);
    if (x->tx_len && x->rx_len && x->tx_len == x->rx_len &&
        (x->flags & 1u)) // full duplex: rx clocked in while tx goes out
      st = hal_spi_transmit_receive(hw, (const uint8_t *)x->tx,
                                    (uint8_t *)x->rx, (uint16_t)x->tx_len,
                                    SPI_TIMEOUT_MS);
    else {
      if (x->tx_len)
        st = hal_spi_transmit(hw, (const uint8_t *)x->tx, (uint16_t)x->tx_len,
                              SPI_TIMEOUT_MS);
      if (st == HAL_OK && x->rx_len)
        st = hal_spi_receive(hw, (uint8_t *)x->rx, (uint16_t)x->rx_len,
                             SPI_TIMEOUT_MS);
    }
    hal_gpio_write(d->cs, HAL_GPIO_HIGH);
  }
  if (st == HAL_OK) {
    finish(m, (int)(x->rx_len ? x->rx_len : x->tx_len));
    return;
  }
  m->recoveries++;
  m->spi_cur = NULL; // re-initialised on the next transfer
  finish(m, V_PBUS_ERESET);
}

int v_pbus_spi_attach(v_pbus_mgr_t *m, hal_spi_instance_t hw) {
  return mgr_init(m, MGR_SPI, (uint8_t)hw);
}

static int spi_dev_start(void *ctx, const v_pbus_xfer_t *x) {
  v_spi_dev_t *d = (v_spi_dev_t *)ctx;
  return mgr_start(d->m, d, x);
}

int v_pbus_spi_add(v_pbus_mgr_t *m, v_spi_dev_t *d, const char *name) {
  if (!m || m->kind != MGR_SPI || !d)
    return V_PBUS_EINVAL;
  d->m = m;
  hal_gpio_enable_clock(d->cs);
  hal_gpio_write(d->cs, HAL_GPIO_HIGH);
  hal_gpio_set_output_type(d->cs, HAL_GPIO_OTYPE_PUSH_PULL);
  hal_gpio_set_mode(d->cs, HAL_GPIO_MODE_OUTPUT, HAL_GPIO_PULL_NONE);
  return v_pdev_register(name, &m->bus, spi_dev_start, d, 0);
}
#endif // NAVHAL_CONFIG_DRV_SPI

// --- worker -----------------------------------------------------------------
void v_pbus_mgr_step(v_pbus_mgr_t *m, uint32_t ticks) {
  // A DMA read in flight is checked every tick; otherwise sleep until kicked.
  if (v_semaphore_take(m->kick, m->dma ? 1u : ticks) == VA_PASS && m->x &&
      !m->dma) {
#if NAVHAL_CONFIG_DRV_I2C
    if (m->kind == MGR_I2C)
      i2c_run(m, m->x);
#endif
#if NAVHAL_CONFIG_DRV_SPI
    if (m->kind == MGR_SPI)
      spi_run(m, m->dev, m->x);
#endif
  }
#if NAVHAL_CONFIG_DRV_I2C && NAVHAL_CONFIG_DRV_I2C_DMA
  if (m->dma && v_get_ticks() - m->t0 > m->timeout) {
    uint32_t s = ENTER_CRITICAL_FROM_ISR();
    int late = m->dma; // the IRQ may have just finished it
    m->dma = 0;
    EXIT_CRITICAL_FROM_ISR(s);
    if (late) {
      // Stream off first, so no completion can land on the next job.
      i2c_dma_stop(m);
      i2c_recover(m);
      finish(m, V_PBUS_ERESET);
    }
  }
#endif
}
#endif // NAVHAL && VAIOS_DEVFS && (I2C || SPI)
