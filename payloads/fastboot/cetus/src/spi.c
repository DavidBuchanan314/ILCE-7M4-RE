#include "io.h"
#include "spi.h"

#define SSP_BASE        0xF1017000ull
#define SSP_DR          (SSP_BASE + 0x08)
#define SSP_SR          (SSP_BASE + 0x0C)


#define SSP_SR_TFE      BIT(0)
#define SSP_SR_TNF      BIT(1)
#define SSP_SR_RFF      BIT(3)
#define SSP_SR_BSY      BIT(4)

/*
 * Iterations, not time: no timer is set up in a payload. Must expire well
 * inside the master's own 50 ms deadline; legitimate waits inside a command
 * are microseconds, since the master is clocking throughout.
 */
#define SPI_POLL_LIMIT  20000u

static int wait_flag(u32 mask, int want_set)
{
    u32 n;

    for (n = 0; n < SPI_POLL_LIMIT; n++) {
        u32 sr = read32(SSP_SR);

        if (want_set ? (sr & mask) : !(sr & mask))
            return SPI_OK;
    }
    return SPI_TIMEOUT;
}

static void send_pair(u8 data, u8 tag)
{
    write32(SSP_DR, 0xFFFF);
    write32(SSP_DR, (u32)data | ((u32)tag << 8));
}

void spi_arm(u8 data, u8 tag)
{
    /* Bounded: a controller left mid-frame can hold BSY indefinitely, and
     * queuing only needs FIFO space anyway. */
    (void)wait_flag(SSP_SR_BSY, 0);

    while (!(read32(SSP_SR) & SSP_SR_TNF))
        ;
    write32(SSP_DR, 0xFFFF);
    while (!(read32(SSP_SR) & SSP_SR_TNF))
        ;
    write32(SSP_DR, (u32)data | ((u32)tag << 8));

    /* Unbounded: waiting for the master to poll is the idle state. */
    while (!(read32(SSP_SR) & SSP_SR_TFE))
        ;
}

/*
 * A 0xFFFF filler goes out ahead of every word. The master polls, so it sees
 * the filler on one exchange and the real word on the next; sending only the
 * word would make a poll that arrives early consume it as a stale repeat.
 */
int spi_send_word(u8 data, u8 tag)
{
    if (wait_flag(SSP_SR_BSY, 0) != SPI_OK)
        return SPI_TIMEOUT;
    send_pair(data, tag);
    return wait_flag(SSP_SR_TFE, 1);
}

int spi_send_stream(u8 data, u8 tag)
{
    if (wait_flag(SSP_SR_TNF, 1) != SPI_OK)
        return SPI_TIMEOUT;
    write32(SSP_DR, (u32)data | ((u32)tag << 8));
    return SPI_OK;
}

/* The master sends all eight words back to back, so wait for a full FIFO. */
int spi_recv_frame(u8 *buf)
{
    u32 i;

    if (wait_flag(SSP_SR_RFF, 1) != SPI_OK)
        return SPI_TIMEOUT;
    if (wait_flag(SSP_SR_BSY, 0) != SPI_OK)
        return SPI_TIMEOUT;

    for (i = 0; i < SPI_FRAME; i += 2) {
        u32 w = read32(SSP_DR);

        buf[i]     = (u8)w;
        buf[i + 1] = (u8)(w >> 8);
    }
    return SPI_OK;
}

/*
 * Words the master clocked while polling have to go before the next frame is
 * read, or it starts mid-stream. The ROM drains after each ready word and ack
 * for the same reason.
 */
void spi_drain(void)
{
    u32 i;

    (void)wait_flag(SSP_SR_BSY, 0);
    for (i = 0; i < 8; i++)
        (void)read32(SSP_DR);
}

/*
 * Eight unconditional reads empty an eight-deep FIFO. Dropping SSE to flush
 * instead can leave BSY asserted, with nothing to clear it.
 */
void spi_resync(void)
{
    spi_drain();
}
