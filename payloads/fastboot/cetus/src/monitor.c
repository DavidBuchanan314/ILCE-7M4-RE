#include "io.h"
#include "spi.h"
#include "monitor.h"
#include "ospi.h"
#include "mbox.h"

#define RDY_DATA        0xC3
#define RDY_TAG         0x10
#define ACK_DATA        0x3A
#define ACK_TAG         0x20
#define RES_TAG         0x30

static u8 ack_seq;
static u8 res_seq;

static u8 seq_next(u8 seq)
{
    return (u8)((seq & 0xF0) | ((seq + 1) & 0x0F));
}

/*
 * Sticky: once a send has timed out the rest of the response is pointless,
 * but it still has to run to completion rather than return early, because the
 * byte count is already on the wire. The loop resynchronises afterwards.
 */
static int res_failed;

static void send_res_byte(u8 b)
{
    if (res_failed)
        return;
    if (spi_send_stream(b, res_seq) != SPI_OK) {
        res_failed = 1;
        return;
    }
    res_seq = seq_next(res_seq);
}

static u32 frame_get32(const u8 *f, u32 off)
{
    return (u32)f[off] | ((u32)f[off + 1] << 8) |
           ((u32)f[off + 2] << 16) | ((u32)f[off + 3] << 24);
}

static void send_response(u8 status, u32 count)
{
    u8 res[SPI_FRAME];
    u32 i;

    for (i = 0; i < SPI_FRAME; i++)
        res[i] = 0;
    res[0] = status;
    res[4] = (u8)count;
    res[5] = (u8)(count >> 8);
    res[6] = (u8)(count >> 16);
    res[7] = (u8)(count >> 24);

    for (i = 0; i < SPI_FRAME; i++)
        send_res_byte(res[i]);
}

static u32 width_of(u8 unit)
{
    return (unit > 2) ? 0 : (1u << unit);
}

static void do_upload(const u8 *cmd)
{
    u32 width = width_of(cmd[1]);
    u32 addr = frame_get32(cmd, 8);
    u32 count = frame_get32(cmd, 12);
    u32 i;

    if (width == 0 || count == 0 || (addr & (width - 1))) {
        send_response(RES_ERR, 0);
        return;
    }

    send_response(RES_OK, count * width);

    for (i = 0; i < count; i++) {
        u64 a = (u64)addr + (u64)i * width;
        u32 v = (width == 4) ? read32(a) :
                (width == 2) ? *(volatile u16 *)a : read8(a);
        u32 k;

        for (k = 0; k < width; k++)
            send_res_byte((u8)(v >> (8 * k)));
    }
}

static void do_download(const u8 *cmd)
{
    u32 width = width_of(cmd[1]);
    u32 addr = frame_get32(cmd, 8);
    u32 count = frame_get32(cmd, 12);
    u32 done = 0;

    if (width == 0 || count == 0 || (addr & (width - 1))) {
        send_response(RES_ERR, 0);
        return;
    }

    while (done < count) {
        u8 chunk[SPI_FRAME];
        u32 i;

        if (spi_send_word(ACK_DATA, ack_seq) != SPI_OK) {
            res_failed = 1;
            return;
        }
        ack_seq = seq_next(ack_seq);
        spi_drain();

        if (spi_recv_frame(chunk) != SPI_OK) {
            res_failed = 1;
            return;
        }

        for (i = 0; i + width <= SPI_FRAME && done < count; i += width) {
            u64 a = (u64)addr + (u64)done * width;
            u32 v = 0, k;

            for (k = 0; k < width; k++)
                v |= (u32)chunk[i + k] << (8 * k);

            if (width == 4)
                write32(a, v);
            else if (width == 2)
                *(volatile u16 *)a = (u16)v;
            else
                *(volatile u8 *)a = (u8)v;
            done++;
        }
    }

    send_response(RES_OK, 0);
}

/*
 * Flash straight onto the wire. The window is only there to bound how much
 * has to be buffered at once -- the response is one continuous byte stream,
 * so a caller can pull the whole 64 MiB in a single command.
 */
#define NOR_WINDOW 4096

static u8 nor_buf[NOR_WINDOW];

static void do_nor_read(const u8 *cmd)
{
    u32 offset = frame_get32(cmd, 8);
    u32 len = frame_get32(cmd, 12);
    u32 done = 0;

    if (len == 0 || (len & 3) || (offset & 3) ||
        offset >= FLASH_SIZE || len > FLASH_SIZE - offset) {
        send_response(RES_ERR, 0);
        return;
    }

    send_response(RES_OK, len);

    while (done < len) {
        u32 chunk = len - done;
        u32 i;

        if (chunk > NOR_WINDOW)
            chunk = NOR_WINDOW;

        if (ospi_indirect_read(offset + done, (u64)(unsigned long)nor_buf,
                               chunk) != 0) {
            /* Mid stream; the length is already committed, so keep the byte
             * count honest and let the caller notice the padding. */
            for (i = 0; i < chunk; i++)
                send_res_byte(0);
        } else {
            for (i = 0; i < chunk; i++)
                send_res_byte(nor_buf[i]);
        }
        done += chunk;
    }
}

static void do_nor_cmd(const u8 *cmd)
{
    u8 buf[8];
    u32 len = cmd[3];
    u32 i;

    if (len == 0 || len > sizeof(buf)) {
        send_response(RES_ERR, 0);
        return;
    }

    if (ospi_stig(cmd[1], cmd[2], (u8)len, frame_get32(cmd, 8),
                  frame_get32(cmd, 12) & 1, buf) != 0) {
        send_response(RES_ERR, 0);
        return;
    }

    send_response(RES_OK, len);
    for (i = 0; i < len; i++)
        send_res_byte(buf[i]);
}

void monitor_loop(void)
{
    u8 cmd[SPI_FRAME];

    for (;;) {
        u8 rdy_seq = RDY_TAG;

        ack_seq = ACK_TAG;
        res_seq = RES_TAG;
        res_failed = 0;

        /* Blocks until the master polls. That wait is the idle state, so it
         * is the one place with no deadline. */
        mbox_step(STEP_ARM1);
        spi_arm(RDY_DATA, rdy_seq);
        rdy_seq = seq_next(rdy_seq);
        mbox_step(STEP_ARM2);
        spi_arm(RDY_DATA, rdy_seq);

        mbox_step(STEP_DRAIN);
        spi_drain();

        /* From here the master is mid-command, so every wait has a deadline
         * and missing one means starting the exchange over. */
        mbox_step(STEP_RECV);
        if (spi_recv_frame(cmd) != SPI_OK) {
            mbox_step(STEP_RECV_TMO);
            spi_resync();
            mbox_step(STEP_RESYNCED);
            continue;
        }

        mbox_step(STEP_ACK);
        if (spi_send_word(ACK_DATA, ack_seq) != SPI_OK) {
            spi_resync();
            continue;
        }
        ack_seq = seq_next(ack_seq);
        spi_drain();

        mbox.cmd = cmd[0];
        mbox_step(STEP_DISPATCH);

        switch (cmd[0]) {
        case OP_STATUS:   send_response(RES_OK, 0); break;
        case OP_UPLOAD:   do_upload(cmd);           break;
        case OP_DOWNLOAD: do_download(cmd);         break;
        case OP_NOR_READ: do_nor_read(cmd);         break;
        case OP_NOR_CMD:  do_nor_cmd(cmd);          break;
        default:          send_response(RES_ERR, 0); break;
        }

        if (res_failed)
            spi_resync();
    }
}
