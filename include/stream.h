#ifndef VAIOS_STREAM_H
#define VAIOS_STREAM_H

// Byte streams over a shared serial port -- USB CDC, a UART -- as a device
// node. Any number of tasks write; one task at a time reads.
//
//   board:  static v_stream_t link;
//           v_stream_cdc_attach(&link, "/dev/cdc0");  // at boot; stream_hw.h
//           task_create_privileged(link_writer, &link, 1024, 6, "cdcw");
//           static void link_writer(void *s) { for (;;) v_stream_writer_step(s, 100); }
//   task:   int fd = v_file_open("/dev/cdc0", VFS_O_RDWR);
//           v_file_write(fd, buf, n);   // queued; returns how much was taken
//           v_file_wait(fd, 10);        // park until bytes arrive
//           n = v_file_read(fd, buf, sizeof buf);
//
// A write only queues: the hardware write runs in the writer task, in thread
// mode, because a CDC write waits for the host and a UART DMA write for its
// completion IRQ, and neither can be waited on inside a syscall. When the far
// end is gone (USB unplugged, port closed), writes are dropped and counted,
// so a host that stops reading never stalls the tasks that write.
//
// A second open for reading fails with V_STREAM_EBUSY until the reader closes
// (or exits). Opens for writing only are unlimited.

#include <stdint.h>

#if VAIOS_DEVFS
#include "ipc.h"

#ifndef V_STREAM_TX
#define V_STREAM_TX 512 // queued output, bytes
#endif
#ifndef V_STREAM_RX
#define V_STREAM_RX 256 // received, not yet read, bytes
#endif
#define V_STREAM_MAX 4  // registered streams, system-wide

#define V_STREAM_EBUSY (-16) // someone else is the reader
#define V_STREAM_EINVAL (-22)

typedef struct v_stream v_stream_t;
struct v_stream {
  // --- backend, set by an attach ---
  // Writer task: send n bytes, returning once the buffer may be reused. < 0 on
  // failure (the bytes are dropped).
  int (*tx)(v_stream_t *s, const uint8_t *p, uint32_t n);
  // Non-blocking read of what the backend holds; NULL = the bytes arrive via
  // v_stream_rx_push_isr into this stream's own ring.
  uint32_t (*rx)(v_stream_t *s, uint8_t *p, uint32_t n);
  int (*up)(v_stream_t *s); // the far end is there; NULL = always
  void *hw;                 // backend's own
  // --- stats ---
  uint32_t dropped;    // written while down, or the queue full
  uint32_t tx_errors;  // tx() failures
  uint32_t rx_dropped; // received with the ring full
  // --- private ---
  const char *name;
  uint8_t txq[V_STREAM_TX] __attribute__((aligned(32)));
  uint8_t rxq[V_STREAM_RX] __attribute__((aligned(32)));
  volatile uint16_t tx_head, tx_tail, rx_head, rx_tail;
  volatile uint8_t reader;
  StaticSemaphore_t kick_buf __attribute__((aligned(8)));
  StaticSemaphore_t rx_buf __attribute__((aligned(8)));
  StaticSemaphore_t done_buf __attribute__((aligned(8)));
  SemaphoreHandle_t kick, rx_ready, tx_done;
};

// Privileged, at boot: make `s` (backend set) the node `name`, e.g. "/dev/cdc0".
int v_stream_register(v_stream_t *s, const char *name);
// The writer: wait up to `ticks` for queued output and send it. Loop on it in
// a privileged task, above the stream's writers.
void v_stream_writer_step(v_stream_t *s, uint32_t ticks);
// Backend, ISR context: bytes received. What doesn't fit is counted, dropped.
void v_stream_rx_push_isr(v_stream_t *s, const uint8_t *p, uint32_t n);
// Backend, ISR context: received data is ready (a backend with its own rx()).
void v_stream_rx_signal_isr(v_stream_t *s);
// Backend, ISR context: the tx() in flight has completed.
void v_stream_tx_done_isr(v_stream_t *s);

#endif // VAIOS_DEVFS
#endif
