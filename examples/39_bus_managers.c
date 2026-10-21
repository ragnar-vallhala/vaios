/*
 * 39 -- bus managers on the NAVIXSM-F401RE (hardware only).
 *
 * Unprivileged tasks share the board's buses through device names; none of
 * them can address the bus itself:
 *   imu   ICM-42688-P on SPI1 (CS PA4)    WHO_AM_I  0x75 -> 0x47, polled
 *   baro  DPS368 on I2C1 0x76 (PB8/PB9)  PROD_ID   0x0D -> 0x10, DMA read
 *   ina   INA226 on I2C1 0x44             MFR_ID    0xFE -> 0x5449, DMA read
 *   rogue opens "baro0" and asks for 0x44: the device pins the address, so it
 *         still reads the DPS368's 0x10
 *   crash submits a transfer and faults with it in flight (KILL policy); the
 *         bus finishes it and moves on
 * Each task reads RUNS times, counts mismatches and errors, and writes its
 * result line to the USB CDC stream /dev/cdc0 -- several writers, one port.
 * Read it on the host's ttyACM.
 *
 * Build with I2C, I2C_DMA, SPI and USB_CDC enabled in NavHAL, and the console
 * routed to the UART (the CDC port is this example's).
 *
 * On any other board (a Nucleo: nothing on the buses) the same tasks run with
 * output on the console, and the checks invert: every I2C read must fail and
 * be recovered (i2c err = rec = all reads), the rogue's read fails too, and
 * the bus must still be serving after the crash task is killed.
 */
#ifndef NAVHAL
#error "NAVHAL is required for this example"
#endif
#include "navhal.h"
#include "pbus_hw.h"
#include "stream_hw.h"
#include "task.h"
#include "utils.h"
#include "vaios.h"
#include "vfile.h"
#include "vfs.h"
#include <stdint.h>

#if !NAVHAL_CONFIG_DRV_I2C || !NAVHAL_CONFIG_DRV_SPI
#error "needs NavHAL I2C and SPI"
#endif

#if NAVHAL_CONFIG_BOARD_NAVIXSM_F401RE
#if !NAVHAL_CONFIG_DRV_USB_CDC || NAVHAL_CONFIG_CONSOLE_ROUTE_CDC
#error "NAVIXSM: needs NavHAL USB_CDC, and the console off CDC"
#endif
#define HAVE_SENSORS 1
#define RUNS 2000u
#else
#define HAVE_SENSORS 0
#define RUNS 50u // each read NACKs: ~1e6-spin HAL timeout, then a bus reset
#endif
#define STK 2048

extern void v_print(const char *s); /* polling console write (privileged) */

static v_pbus_mgr_t i2c1, spi1;
static v_spi_dev_t imu_dev = {
    .cs = GPIO_PA04,
    .cfg = {.cpol = HAL_SPI_CPOL_LOW,
            .cpha = HAL_SPI_CPHA_1EDGE,
            .datasize = HAL_SPI_DATASIZE_8BIT,
            .firstbit = HAL_SPI_FIRSTBIT_MSB},
    .hz = 1000000u,
};
#if HAVE_SENSORS
static v_stream_t cdc;
#define OUT_OPEN() v_file_open("/dev/cdc0", VFS_O_WRONLY)
#define CDC_DROPPED cdc.dropped
#else
#define OUT_OPEN() 1 // the console
#define CDC_DROPPED 0u
#endif

// `out` is the calling task's own fd: an unprivileged task has no writable
// globals (they sit in kernel RAM), so nothing here may be static.
static void say(int out, const char *fmt, int a, int b, int c) {
  char buf[96];
  int n = print_fmt_buf(buf, sizeof buf, fmt, a, b, c);
  for (int done = 0; done < n;) {
    int w = v_file_write(out, buf + done, (uint32_t)(n - done));
    if (w <= 0) {
      v_delay(2);
      continue;
    }
    done += w;
  }
}

// Read register `reg` (n bytes, big-endian) RUNS times; expect `want`.
static void check_reg(const char *dev, const char *tag, uint8_t reg,
                      uint32_t n, uint32_t want) {
  int out = OUT_OPEN();
  int fd = v_pbus_open(dev);
  uint32_t bad = 0, err = 0, last = 0;
  for (uint32_t i = 0; i < RUNS && fd >= 0; i++) {
    uint8_t rx[2] = {0};
    int rc = v_pdev_read_reg(fd, reg, rx, n, 2, 20);
    if (rc < 0) {
      err++;
      continue;
    }
    last = n == 2 ? (uint32_t)(rx[0] << 8 | rx[1]) : rx[0];
    bad += last != want;
  }
  say(out, tag, (int)last, (int)bad, (int)err);
  if (fd < 0)
    say(out, "[bm] open failed\r\n", 0, 0, 0);
}

static void imu(void *arg) {
  (void)arg;
  // SPI: bit 7 set = read. The part answers on the second byte.
  check_reg("imu0", "[bm] imu who=0x%x bad=%u err=%u\r\n", 0x75 | 0x80, 1,
            0x47);
  for (;;)
    v_delay(1000);
}

static void baro(void *arg) {
  (void)arg;
  check_reg("baro0", "[bm] baro id=0x%x bad=%u err=%u\r\n", 0x0D, 1, 0x10);
  for (;;)
    v_delay(1000);
}

static void ina(void *arg) {
  (void)arg;
  check_reg("ina0", "[bm] ina mfr=0x%x bad=%u err=%u\r\n", 0xFE, 2, 0x5449);
  for (;;)
    v_delay(1000);
}

static void rogue(void *arg) {
  (void)arg;
  int out = OUT_OPEN();
  int fd = v_pbus_open("baro0");
  uint8_t reg = 0x0D, id = 0;
  v_pbus_xfer_t x = {.addr = 0x44, .tx = &reg, .tx_len = 1, .rx = &id,
                     .rx_len = 1};
  int rc = v_pbus_xfer(fd, &x, 1, 20);
  say(out, "[bm] rogue asked 0x44, got 0x%x rc=%d\r\n", id, rc, 0);
  for (;;)
    v_delay(1000);
}

static void crash(void *arg) {
  (void)arg;
  int out = OUT_OPEN();
  int fd = v_pbus_open("ina0");
  uint8_t reg = 0xFE, rx[2];
  v_pbus_xfer_t x = {.tx = &reg, .tx_len = 1, .rx = rx, .rx_len = 2};
  v_pbus_xfer_submit(fd, &x, 3);
  say(out, "[bm] crash faults mid-transfer\r\n", 0, 0, 0);
  (void)*(volatile uint32_t *)0x20000000u; // kernel RAM: MemManage -> killed
  say(out, "[bm] BUG: crash survived\r\n", 0, 0, 0);
  for (;;)
    v_delay(1000);
}

static void i2c_worker(void *m) {
  for (;;)
    v_pbus_mgr_step(m, 100);
}
#if HAVE_SENSORS
static void cdc_writer(void *s) {
  for (;;)
    v_stream_writer_step(s, 100);
}
#endif

static void monitor(void *arg) {
  (void)arg;
  int fd = OUT_OPEN();
  for (;;) {
    task_delay(3000);
    char buf[128];
    int n = print_fmt_buf(
        buf, sizeof buf,
        "[bm] i2c err=%u rec=%u  spi err=%u rec=%u  cdc drop=%u faults=%u\r\n",
        (unsigned)i2c1.errors, (unsigned)i2c1.recoveries,
        (unsigned)spi1.errors, (unsigned)spi1.recoveries,
        (unsigned)CDC_DROPPED, (unsigned)v_mpu_fault_stats.faults);
    v_file_write(fd, buf, (uint32_t)n);
  }
}

int main(void) {
  vaios_init_config_t cfg = {.internal_clock_setup = 1,
                             .internal_sd_card_setup = 0};
  v_system_init(&cfg);

  // I2C1 on PB8/PB9 (NavHAL's own table says PB6/PB7: board #242).
  hal_gpio_pin_t pins[2] = {GPIO_PB08, GPIO_PB09};
  for (int i = 0; i < 2; i++) {
    hal_gpio_enable_clock(pins[i]);
    hal_gpio_set_alternate_function(pins[i], GPIO_FUNC_I2C);
    hal_gpio_set_output_type(pins[i], HAL_GPIO_OTYPE_OPEN_DRAIN);
    hal_gpio_set_output_speed(pins[i], HAL_GPIO_SPEED_VERY_HIGH);
    hal_gpio_set_mode(pins[i], HAL_GPIO_MODE_AF, HAL_GPIO_PULL_UP);
  }
  hal_i2c_config_t i2c_cfg = {.clock_speed = HAL_I2C_SPEED_FAST,
                              .acknowledge = true};
  if (v_pbus_i2c_attach(&i2c1, HAL_I2C_1, &i2c_cfg, GPIO_PB08, GPIO_PB09,
                        GPIO_FUNC_I2C) ||
      v_pbus_i2c_add(&i2c1, "baro0", 0x76) || v_pbus_i2c_add(&i2c1, "ina0", 0x44))
    v_print("[bm] BUG: i2c setup\r\n");
  if (v_pbus_spi_attach(&spi1, HAL_SPI_1) ||
      v_pbus_spi_add(&spi1, &imu_dev, "imu0"))
    v_print("[bm] BUG: spi setup\r\n");
#if HAVE_SENSORS
  if (v_stream_cdc_attach(&cdc, "/dev/cdc0"))
    v_print("[bm] BUG: cdc setup\r\n");
#endif

  // Workers above their users; users at 2..3.
  task_create_privileged(i2c_worker, &i2c1, STK, 6, "i2c1");
  task_create_privileged(i2c_worker, &spi1, STK, 6, "spi1");
#if HAVE_SENSORS
  task_create_privileged(cdc_writer, &cdc, STK, 5, "cdcw");
#endif
  task_create_privileged(monitor, NULL, STK, 1, "monitor");
  task_create_named(imu, NULL, STK, 2, "imu");
  task_create_named(baro, NULL, STK, 2, "baro");
  task_create_named(ina, NULL, STK, 3, "ina");
  task_create_named(rogue, NULL, STK, 2, "rogue");
  uint32_t c = task_create_named(crash, NULL, STK, 3, "crash");
  task_set_fault_policy(c, V_FAULT_KILL, 0);
  scheduler_start();
  for (;;)
    ;
}
