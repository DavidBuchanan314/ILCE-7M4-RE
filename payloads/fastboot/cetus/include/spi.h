#ifndef CETUS_SPI_H
#define CETUS_SPI_H

#include "io.h"

/*
 * Slave side of the mask ROM's SPI monitor protocol.
 *
 * A payload is entered from the monitor, so the controller is already a
 * 16-bit mode-3 slave with the AP driving the clock. Continuing to speak the
 * same framing means the AP's existing client keeps working and the link
 * survives the hand-off -- the payload simply answers more opcodes.
 *
 * Word layout is [7:0] data, [15:8] tag. Each tag carries a sequence nibble
 * that restarts at the top of every command.
 */

#define SPI_FRAME       16

#define SPI_OK          0
#define SPI_TIMEOUT     (-1)

/*
 * The ready word, and the only wait here that is allowed to last forever.
 * Between commands the master is not clocking at all, so blocking until it
 * does is the idle state rather than a fault.
 */
void spi_arm(u8 data, u8 tag);

/*
 * Everything below runs inside a command, where the master is actively
 * clocking. A wait that expires there means the two ends disagree about
 * where the frame boundary is, and the only way out is to resynchronise --
 * so these report it rather than hanging the processor.
 */
int spi_send_word(u8 data, u8 tag);

/*
 * Bulk form: queue on FIFO space rather than waiting for an idle bus, and
 * without the 0xFFFF filler. The filler costs a whole frame per byte and only
 * exists so a poll landing early sees something recognisable -- the rolling
 * tag already does that. Waiting for idle here would also stall a master that
 * keeps the bus busy, which is the point of streaming in the first place.
 */
int spi_send_stream(u8 data, u8 tag);

int spi_recv_frame(u8 *buf);
void spi_drain(void);

/* Drop whatever is half-shifted and start listening again from a clean FIFO. */
void spi_resync(void);

#endif /* CETUS_SPI_H */
