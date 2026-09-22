#ifndef CETUS_SPI_H
#define CETUS_SPI_H

#include "io.h"

/*
 * Slave side of the mask ROM's SPI monitor protocol. Entered from the monitor,
 * so the controller is already a 16-bit mode-3 slave with the AP clocking.
 *
 * Word layout is [7:0] data, [15:8] tag, each tag carrying a sequence nibble
 * that restarts at the top of every command.
 */

#define SPI_FRAME       16

#define SPI_OK          0
#define SPI_TIMEOUT     (-1)

/*
 * The ready word. Blocks forever: between commands the master is not clocking
 * at all, so waiting is the idle state.
 */
void spi_arm(u8 data, u8 tag);

/*
 * Inside a command the master is actively clocking, so an expired wait means
 * the two ends disagree about the frame boundary. These report rather than
 * hang.
 */
int spi_send_word(u8 data, u8 tag);

/*
 * Bulk form: queues on FIFO space, with no 0xFFFF filler -- the rolling tag
 * already tells an early poll from a real word.
 */
int spi_send_stream(u8 data, u8 tag);

int spi_recv_frame(u8 *buf);
void spi_drain(void);

/* Drop whatever is half-shifted and start listening again from a clean FIFO. */
void spi_resync(void);

#endif /* CETUS_SPI_H */
