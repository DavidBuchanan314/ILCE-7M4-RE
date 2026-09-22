#include "io.h"
#include "timer.h"
#include "darwin.h"

/*
 * SIO transport for the Darwin link, transcribed from the AP loader's
 * transaction primitive at 0xFE04DD3C. SIO+0x100 is one 0x80-byte buffer used
 * in both directions.
 *
 * Retrying is the protocol, not error handling: Darwin answers a command in a
 * later exchange, and returns a CRC of 0xFFFF for "nothing ready yet".
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
 * Rate index, from the loader's runtime config word (0xFE06A934/938/93C/940),
 * which also fixes channel 0 and GPIO port 4.
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
 * The SIO clock and data pads share GPIO port 4 with XCS, and are left muxed
 * to GPIO by both the bootrom and the loader's teardown. Clearing FUNC returns
 * them to their hardware function; bit 2 is not in the mask because XCS is
 * driven by us and stays GPIO.
 */
#define SIO_PINS        (BIT(0) | BIT(1) | BIT(3))

/*
 * The loader spins 0x65 and 0x3E9 units of an unidentified tick source. These
 * assume the slower reading of it; setup and hold are minimums.
 */
#define CS_SETUP_US     150u
#define CS_HOLD_US      1200u

/* A 0x80-byte exchange is microseconds of shifting; 20 ms is pure slack. */
#define BUSY_TIMEOUT_TICKS  (TIMER0_HZ / 50)

/* ---- CRC-16/CCITT-FALSE -------------------------------------------------
 *
 * Init 0xFFFF, poly 0x1021, MSB-first, no reflection, no final XOR, over
 * bytes [0x00..0x7D], stored little-endian at 0x7E.
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
    /* Idle high BEFORE handing the pad to the GPIO block, so switching the
     * mux cannot present a spurious falling edge. */
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
             * A validating reply is not necessarily ours: a call that timed
             * out leaves its reply unconsumed. Both handlers leave the request
             * header intact in the buffer they reply from (0x16 bzeroes only
             * from +6), so match_len pins it to this command.
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


/* The address field is LITTLE-endian. */
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

    /* Darwin answers in place, so the payload comes back at PKT_HDR. */
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
 * A reload of 0x0A means the AP populated Darwin's config during a normal boot
 * this power cycle; 0x00 means it did not, and makes every vwdt_set a no-op.
 * A counter of 0xFF is the disabled sentinel; 0x00 means the tick is not
 * running, since a live tick turns 0x00 into 0xFF within one 100 ms period.
 *
 * To disarm a channel: 83 * 0x78 = 0x26E8, so the table sits 0x14 bytes into
 * block 83 and a 0x18-byte command 0x12 write lands it as that block's tail.
 */
int darwin_vwdt_state(u8 counters[4], u8 reloads[3])
{
    if (counters && darwin_read(VWDT_TABLE, counters, 4) != 0)
        return -1;
    if (reloads && darwin_read(VWDT_RELOAD, reloads, 3) != 0)
        return -1;
    return 0;
}
