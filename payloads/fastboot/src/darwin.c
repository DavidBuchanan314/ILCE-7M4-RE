#include "io.h"
#include "timer.h"
#include "darwin.h"

/*
 * SIO transport for the Darwin link.
 *
 * Every register, value and delay below is read out of the AP's own
 * transaction primitive at 0xFE04DD3C in loader.bin, which runs as
 *
 *      sio_xfer(tx, rx, timeout, check_crc, expected_cmd)
 *
 * and does, per attempt:
 *
 *      SIO[0x10] = 0x01000000              ; enable
 *      SIO[0x14] = (rate & 0xff) << 24     ; rate, split across two regs
 *      SIO[0x18] = (rate << 16) & 0xff000000
 *      SIO[0x00] = 0xD8000000
 *      SIO[0x04] = 0x30000000
 *      XCS low                             ; GPIO port 4 bit 2, active low
 *      wait 0x65 units
 *      SIO[0x08] = 0
 *      SIO[0x0C] = 0x7F000000              ; 0x80 bytes - 1, in bits 31:24
 *      memcpy(SIO+0x100, tx, 0x80)
 *      SIO[0x00] |= 0x20000000             ; go
 *      while (SIO[0x00] & (1 << 29)) ;     ; bit 29 = busy
 *      memcpy(rx, SIO+0x100, 0x80)         ; SAME buffer, both directions
 *      XCS high
 *      wait 0x3E9 units
 *      SIO[0x00]  = 0xC0000000
 *      SIO[0x00] &= ~0x20000000
 *      SIO[0x10] = 0
 *
 * then validates the reply's CRC and command byte and RETRIES the whole
 * exchange until they match or the caller's timeout expires. That retry is
 * the protocol, not error handling: Darwin answers a command in a later
 * exchange, and returns a CRC of 0xFFFF for "nothing ready yet".
 */

#define SIO_CH          0u
#define SIO_BASE        (0xF1005000ull + (u64)SIO_CH * 0x1000)

#define SIO_CTRL        (SIO_BASE + 0x00)
#define SIO_CTRL2       (SIO_BASE + 0x04)
#define SIO_REG08       (SIO_BASE + 0x08)
#define SIO_LEN         (SIO_BASE + 0x0C)
#define SIO_CFG         (SIO_BASE + 0x10)
#define SIO_RATE_LO     (SIO_BASE + 0x14)
#define SIO_RATE_HI     (SIO_BASE + 0x18)
#define SIO_DATA        (SIO_BASE + 0x100)

#define SIO_CTRL_SETUP  0xD8000000u
#define SIO_CTRL_GO     0x20000000u
#define SIO_CTRL_BUSY   BIT(29)
#define SIO_CTRL_STOP   0xC0000000u
#define SIO_CTRL2_VAL   0x30000000u
#define SIO_CFG_ON      0x01000000u
#define SIO_LEN_VAL     ((u32)(DARWIN_PKT - 1) << 24)

/*
 * Rate index, from the loader's runtime config word. The boot log prints it
 * as `PWD:SIO C[0] P[4] R[3]` -- channel 0, GPIO port 4, rate 3 -- and those
 * are the same three globals the primitive loads (0xFE06A934/938/93C/940).
 */
#define SIO_RATE        3u

/* XCS: GPIO port 4 bit 2, active low. SET/CLR only -- never touch WDATA. */
#define XCS_PORT        4u
#define XCS_BIT         2u
#define XCS_GPIO        (0xF101D000ull + 0x1000ull * XCS_PORT)
#define XCS_DIR_SET     (XCS_GPIO + 0x04)
#define XCS_FUNC_SET    (XCS_GPIO + 0x34)
#define XCS_FUNC_CLR    (XCS_GPIO + 0x38)
#define XCS_SET         (XCS_GPIO + 0x44)   /* idle high   */
#define XCS_CLR         (XCS_GPIO + 0x48)   /* assert low  */

/*
 * The SIO clock and data pads share GPIO port 4 with XCS, and both the
 * bootrom and the loader's teardown ("PWR:Disable SIO ports :set IN-PUP")
 * leave the whole port muxed to GPIO -- FUNC reads 0xFFF. With FUNC set,
 * software owns the pad, so the SIO block shifts against nothing and every
 * reply reads back as zeros even though the block itself is running.
 *
 * Clearing FUNC returns these three to their hardware function. Bit 2 is
 * deliberately NOT in the mask: XCS is driven by us and has to stay GPIO.
 */
#define SIO_PINS        (BIT(0) | BIT(1) | BIT(3))

/*
 * The loader spins on its own tick source for 0x65 and 0x3E9 units around the
 * chip select. That source is not the one timer.h describes and its rate is
 * not established, so these are chosen to satisfy the larger reading (1 unit
 * = 1 us) with margin. Chip-select setup and hold are minimums; being slow
 * costs only throughput.
 */
#define CS_SETUP_US     150u
#define CS_HOLD_US      1200u

/* A 0x80-byte exchange is microseconds of shifting; 20 ms is pure slack. */
#define BUSY_TIMEOUT_TICKS  (TIMER0_HZ / 50)

/* ---- CRC-16/CCITT-FALSE -------------------------------------------------
 *
 * Init 0xFFFF, poly 0x1021, MSB-first, no reflection, no final XOR, over
 * bytes [0x00..0x7D], stored little-endian at 0x7E. The AP computes it in
 * software at 0xFE04D43C; Darwin uses a hardware unit but the same function.
 */
static u16 darwin_crc16(const u8 *p)
{
    u32 crc = 0xFFFF;
    u32 i, b;

    for (i = 0; i < DARWIN_PKT - 2; i++) {
        crc ^= (u32)p[i] << 8;
        for (b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
        crc &= 0xFFFF;
    }
    return (u16)crc;
}

/* ---- one raw exchange ---------------------------------------------------- */

static void sio_load(const u8 *tx)
{
    u32 i;
    for (i = 0; i < DARWIN_PKT; i += 4)
        write32(SIO_DATA + i, (u32)tx[i] | ((u32)tx[i + 1] << 8) |
                              ((u32)tx[i + 2] << 16) | ((u32)tx[i + 3] << 24));
}

static void sio_store(u8 *rx)
{
    u32 i, v;
    for (i = 0; i < DARWIN_PKT; i += 4) {
        v = read32(SIO_DATA + i);
        rx[i]     = (u8)v;
        rx[i + 1] = (u8)(v >> 8);
        rx[i + 2] = (u8)(v >> 16);
        rx[i + 3] = (u8)(v >> 24);
    }
}

static int sio_xfer(const u8 *tx, u8 *rx)
{
    u32 start;

    write32(SIO_CFG,     SIO_CFG_ON);
    write32(SIO_RATE_LO, (SIO_RATE & 0xFFu) << 24);
    write32(SIO_RATE_HI, (SIO_RATE << 16) & 0xFF000000u);
    write32(SIO_CTRL,    SIO_CTRL_SETUP);
    write32(SIO_CTRL2,   SIO_CTRL2_VAL);

    write32(XCS_CLR, BIT(XCS_BIT));
    udelay(CS_SETUP_US);

    write32(SIO_REG08, 0);
    write32(SIO_LEN,   SIO_LEN_VAL);
    sio_load(tx);

    setbits32(SIO_CTRL, SIO_CTRL_GO);

    start = timer_ticks();
    while (read32(SIO_CTRL) & SIO_CTRL_BUSY) {
        if (timer_ticks() - start > BUSY_TIMEOUT_TICKS) {
            /* Release the bus before giving up, or Darwin sees a stuck
             * chip select and the next attempt starts mid-frame. */
            write32(XCS_SET, BIT(XCS_BIT));
            write32(SIO_CTRL, SIO_CTRL_STOP);
            write32(SIO_CFG, 0);
            return -1;
        }
    }

    sio_store(rx);

    write32(XCS_SET, BIT(XCS_BIT));
    udelay(CS_HOLD_US);

    write32(SIO_CTRL, SIO_CTRL_STOP);
    clrbits32(SIO_CTRL, SIO_CTRL_GO);
    write32(SIO_CFG, 0);
    return 0;
}

/* ---- public transport ---------------------------------------------------- */

void darwin_init(void)
{
    /* Idle the chip select high BEFORE handing the pad to the GPIO block,
     * so switching the mux cannot present a spurious falling edge. Same
     * ordering rule as the XRESET_REQ sequence in reset.c. */
    write32(XCS_SET,      BIT(XCS_BIT));
    write32(XCS_DIR_SET,  BIT(XCS_BIT));
    write32(XCS_FUNC_SET, BIT(XCS_BIT));

    /* Hand the clock and data pads back to the SIO block. */
    write32(XCS_FUNC_CLR, SIO_PINS);
    dsb();
}

int darwin_xfer(u8 *tx, u8 *rx, u32 match_len, u32 attempts)
{
    u32 n;
    u16 crc;

    crc = darwin_crc16(tx);
    tx[DARWIN_PKT - 2] = (u8)crc;
    tx[DARWIN_PKT - 1] = (u8)(crc >> 8);

    for (n = 0; n < attempts; n++) {
        if (sio_xfer(tx, rx) == 0) {
            u16 want = darwin_crc16(rx);
            u16 got  = (u16)(rx[DARWIN_PKT - 2] |
                             ((u16)rx[DARWIN_PKT - 1] << 8));
            u32 i;

            /*
             * A CRC of 0xFFFF is Darwin saying "nothing ready yet" -- the
             * normal case on the first exchange, since a reply lands in a
             * later one. Anything else that validates is a real reply, but
             * not necessarily OURS: a call that timed out leaves its reply
             * unconsumed, and the next command would otherwise accept it.
             *
             * Both handlers leave the request header intact in the buffer
             * they reply from (0x16 bzeroes only from +6), so matching the
             * leading `match_len` bytes pins the reply to this exact
             * command, address and length -- not merely to the opcode.
             */
            if (want == got) {
                for (i = 0; i < match_len; i++)
                    if (rx[i] != tx[i])
                        break;
                if (i == match_len)
                    return 0;
            }
        }
    }
    return -1;
}


/* ---- commands ------------------------------------------------------------ */

#define CMD_WRITE_BLOCK 0x12
#define CMD_MEM         0x16

#define PKT_MAX_PAYLOAD 0x78


/* The address field is LITTLE-endian: the shared decoder at file 0x32B44 in
 * darwin_core.bin builds p[0] | p[1]<<8 | p[2]<<16 | p[3]<<24. */
static void put_addr(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

int darwin_read(u32 addr, u8 *buf, u32 len)
{
    u8 tx[DARWIN_PKT], rx[DARWIN_PKT];
    u32 i;

    if (len == 0 || len > PKT_MAX_PAYLOAD)
        return -1;

    for (i = 0; i < DARWIN_PKT; i++)
        tx[i] = 0;
    tx[0] = CMD_MEM;
    put_addr(tx + 1, addr);
    tx[5] = (u8)len;            /* bit 7 clear selects the read path */

    /* Match the whole header: opcode, address and length. */
    if (darwin_xfer(tx, rx, PKT_HDR, DARWIN_ATTEMPTS) != 0)
        return -1;

    /* Darwin answers in place: it zeroes its own rx+6 and copies the
     * requested bytes there, so the payload comes back at the same offset
     * the request's payload would have occupied. */
    for (i = 0; i < len; i++)
        buf[i] = rx[PKT_HDR + i];
    return 0;
}

int darwin_write_block(u32 off, const u8 *buf, u32 len)
{
    u8 tx[DARWIN_PKT], rx[DARWIN_PKT];
    u32 i;

    if (len == 0 || len > PKT_MAX_PAYLOAD)
        return -1;
    if (off % PKT_MAX_PAYLOAD)      /* the handler rejects this with -1 */
        return -1;

    for (i = 0; i < DARWIN_PKT; i++)
        tx[i] = 0;
    tx[0] = CMD_WRITE_BLOCK;
    put_addr(tx + 1, off);
    tx[5] = (u8)len;
    for (i = 0; i < len; i++)
        tx[PKT_HDR + i] = buf[i];

    return darwin_xfer(tx, rx, PKT_HDR, DARWIN_ATTEMPTS);
}

/* ---- the Virtual WDT ----------------------------------------------------- */

/*
 * Reading these two is what says whether the watchdog is armed at all, and it
 * doubles as a way to tell which regime a boot is in: the AP populates this
 * config over command 0x12 during a normal boot, so a reload of 0x0A means a
 * normal boot has happened this power cycle and 0x00 means it has not.
 *
 * A reload of 0 makes every vwdt_set a no-op (the cbz at file 0x221C), so the
 * channels can never be armed. A counter of 0xFF is the disabled sentinel
 * vwdt_tick skips; 0x00 means the tick is not running at all, since a live
 * tick turns 0x00 into 0xFF and stores it back within one 100 ms period.
 *
 * If a channel ever does need disarming, the counter table is reachable by
 * command 0x12 even though it is not block-aligned: 83 * 0x78 = 0x26E8, so
 * the table sits 0x14 bytes into block 83 and a 0x18-byte write lands it as
 * that block's last four bytes.
 */
int darwin_vwdt_state(u8 counters[4], u8 reloads[3])
{
    if (counters && darwin_read(VWDT_TABLE, counters, 4) != 0)
        return -1;
    if (reloads && darwin_read(VWDT_RELOAD, reloads, 3) != 0)
        return -1;
    return 0;
}
