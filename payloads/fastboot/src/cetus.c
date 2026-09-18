#include "io.h"
#include "timer.h"
#include "scu.h"
#include "cetus.h"

#define SPI0_BASE       0xF1017000ull
#define SSP_CR0         (SPI0_BASE + 0x00)
#define SSP_CR1         (SPI0_BASE + 0x04)
#define SSP_DR          (SPI0_BASE + 0x08)
#define SSP_SR          (SPI0_BASE + 0x0C)
#define SSP_CPSR        (SPI0_BASE + 0x10)
#define SSP_IMSC        (SPI0_BASE + 0x14)
#define SSP_ICR         (SPI0_BASE + 0x20)
#define SSP_DMACR       (SPI0_BASE + 0x24)

#define SSP_SR_TFE      BIT(0)
#define SSP_SR_TNF      BIT(1)
#define SSP_SR_RNE      BIT(2)
#define SSP_SR_BSY      BIT(4)

#define SSP_CR0_DSS16   0x0Fu
#define SSP_CR0_SPO     BIT(6)
#define SSP_CR0_SPH     BIT(7)
#define SSP_CR0_SCR(n)  ((u16)((n) << 8))

#define SSP_CR1_SSE     BIT(1)

/*
 * SCK = SSPCLK / (CPSDVSR * (1 + SCR)).
 *
 * The CP's ROM puts its own SPI0 on the 192 MHz source, and a PL022 slave
 * wants SSPCLK at least 12x SCK, so the far end tolerates 16 MHz by the book.
 * Both sides are ours once the payload is up, so the rate is a runtime knob
 * rather than a constant -- every byte read back is checked against a known
 * image, which is what makes sweeping it safe.
 */
#define SSPCLK_SEL_48   0u
#define SSPCLK_SEL_96   1u
#define SSPCLK_SEL_192  2u

static const u32 sspclk_khz[] = { 48000u, 96000u, 192000u, 48000u };

/*
 * 96 MHz / (2 * 3) = 16 MHz: twice the stock rate and still the 12x the
 * PL022 asks of a slave. 32 MHz reads back byte-exact too and is a further
 * ~1.35x (`oem cetus rate:2:2:2`), but that is 6x the far end's clock rather
 * than 12x, and the init-time bring-up runs at whatever this says -- being
 * out of spec there costs the CP entirely. The link dies somewhere between
 * 32 and 48 MHz.
 */
static u32 rate_sel     = SSPCLK_SEL_96;
static u32 rate_cpsdvsr = 2u;
static u32 rate_scr     = 2u;

#define SPI0_GPIO       (0xF101D000ull + 0x1000ull * 1)
#define GPIO_INEN_SET   (SPI0_GPIO + 0x24)
#define GPIO_FUNC_CLR   (SPI0_GPIO + 0x38)
#define SPI0_PINS       (BIT(0) | BIT(1) | BIT(2) | BIT(3))
#define SPI0_PIN_RXD    BIT(0)

#define FIFO_TIMEOUT_TICKS  (TIMER0_HZ / 100)
#define WORD_TIMEOUT_TICKS  (TIMER0_HZ / 20)

/*
 * The slave drains its receive FIFO right after it emits a ready or ack word,
 * so the frames we clock while polling for one are harmless -- but a frame
 * sent too soon after the match lands in that drain and is lost.
 */
#define HANDSHAKE_SETTLE_US 170

#define RDY_DATA        0xC3
#define RDY_TAG         0x10
#define ACK_DATA        0x3A
#define ACK_TAG         0x20
#define RES_TAG         0x30

#define CMD_STATUS      0x11
#define CMD_ENTRY       0x13
#define CMD_DOWNLOAD    0x21
#define CMD_UPLOAD      0x22
#define CMD_NOR_READ    0x30
#define CMD_NOR_CMD     0x32

/* Read Identification, per JEDEC. */
#define OP_RDID         0x9F

static int spi_ready;

void cetus_spi_init(void)
{
    if (spi_ready)
        return;

    write32(SCU_CLKSEL_SPI_CLR, CLKSEL_SPI0_MASK);
    write32(SCU_CLKSEL_SPI_SET, rate_sel);
    write32(SCU_CLK_PERI_SET, CLK_SPI0);
    write32(SCU_RST5_DEASSERT, RST_SPI0_SSP | RST_SPI0_APB);
    udelay(1);

    write32(GPIO_FUNC_CLR, SPI0_PINS);
    write32(GPIO_INEN_SET, SPI0_PIN_RXD);

    write16(SSP_CR1, 0);
    write16(SSP_CR0, SSP_CR0_DSS16 | SSP_CR0_SPO | SSP_CR0_SPH |
                     SSP_CR0_SCR(rate_scr));
    write16(SSP_CPSR, rate_cpsdvsr);
    write16(SSP_IMSC, 0);
    write16(SSP_DMACR, 0);
    write16(SSP_ICR, 3);
    write16(SSP_CR1, SSP_CR1_SSE);
    dsb();

    spi_ready = 1;
}

u32 cetus_rate_khz(void)
{
    return sspclk_khz[rate_sel & 3] / (rate_cpsdvsr * (1 + rate_scr));
}

int cetus_set_rate(u32 sel, u32 cpsdvsr, u32 scr)
{
    if (sel > SSPCLK_SEL_192 || cpsdvsr < 2 || cpsdvsr > 254 ||
        (cpsdvsr & 1) || scr > 255)
        return CETUS_E_ARG;

    rate_sel     = sel;
    rate_cpsdvsr = cpsdvsr;
    rate_scr     = scr;

    /* Force the next command to reprogram the controller. */
    spi_ready = 0;
    cetus_spi_init();
    return 0;
}

u32 cetus_reset_state(void)    { return read32(SCU_CP_RESET); }
u32 cetus_bootmode_state(void) { return read32(SCU_CP_BOOTMODE); }

void cetus_monitor_boot(void)
{
    /* Back to the ROM's sender until our payload is up again. */
    cetus_set_pipelined(0);

    /*
     * Reprogram the controller rather than reusing it. Dropping SSE empties
     * both FIFOs, and without that a link abandoned mid-frame keeps whatever
     * words were in flight -- which shifts every later word by that many and
     * makes the reset below look like it achieved nothing.
     */
    spi_ready = 0;
    cetus_spi_init();

    write32(SCU_CP_RESET, 0);
    write32(SCU_CP_BOOTMODE, 1);
    dsb();
    udelay(1000);
    write32(SCU_CP_RESET, 1);
    dsb();

    mdelay(50);
}

static int fifo_wait(u32 mask, int want_set)
{
    u32 start = timer_ticks();

    for (;;) {
        u32 sr = read16(SSP_SR);
        if (want_set ? (sr & mask) : !(sr & mask))
            return 0;
        if (timer_ticks() - start > FIFO_TIMEOUT_TICKS)
            return -1;
    }
}

static void rx_drain(void)
{
    while (read16(SSP_SR) & SSP_SR_RNE)
        (void)read16(SSP_DR);
}

/*
 * Every word is [7:0] data, [15:8] tag. The tag carries a rolling sequence
 * nibble that the target resets at the top of each command.
 */
static int spi_word(u16 tx, u16 *rx)
{
    if (fifo_wait(SSP_SR_TNF, 1) != 0)
        return -1;
    write16(SSP_DR, tx);

    if (fifo_wait(SSP_SR_RNE, 1) != 0)
        return -1;
    *rx = read16(SSP_DR);
    return 0;
}

/*
 * Eight words pushed back to back, so chip select stays asserted for the whole
 * frame; the target waits for its receive FIFO to fill before it looks at one.
 */
static int spi_send_frame(const u8 *f)
{
    u32 i;

    if (fifo_wait(SSP_SR_BSY, 0) != 0)
        return -1;
    rx_drain();

    for (i = 0; i < CETUS_FRAME; i += 2) {
        if (fifo_wait(SSP_SR_TNF, 1) != 0)
            return -1;
        write16(SSP_DR, (u16)f[i] | ((u16)f[i + 1] << 8));
    }

    if (fifo_wait(SSP_SR_TFE, 1) != 0)
        return -1;
    if (fifo_wait(SSP_SR_BSY, 0) != 0)
        return -1;
    rx_drain();
    return 0;
}

static int wait_word(u8 data, u8 tag)
{
    u32 start = timer_ticks();

    while (timer_ticks() - start <= WORD_TIMEOUT_TICKS) {
        u16 w;
        if (spi_word(0, &w) != 0)
            return -1;
        if ((u8)(w >> 8) == tag && (u8)w == data)
            return 0;
    }
    return -1;
}

/*
 * Each of the three tags carries its own sequence nibble, which the target
 * rolls on every word and resets at the top of a command. Tracking them is
 * what keeps a stale word -- the target repeats its last one until it has
 * another ready -- from being mistaken for the next reply.
 */
static u8 ack_seq;
static u8 res_seq;

static u8 seq_next(u8 seq)
{
    return (u8)((seq & 0xF0) | ((seq + 1) & 0x0F));
}

static int recv_rdy(void)
{
    u32 k;

    for (k = 0; k < 2; k++)
        if (wait_word(RDY_DATA, (u8)(RDY_TAG + k)) != 0)
            return -1;

    ack_seq = ACK_TAG;
    res_seq = RES_TAG;

    udelay(HANDSHAKE_SETTLE_US);
    return 0;
}

static int recv_ack(void)
{
    if (wait_word(ACK_DATA, ack_seq) != 0)
        return -1;
    ack_seq = seq_next(ack_seq);
    udelay(HANDSHAKE_SETTLE_US);
    return 0;
}

/*
 * How many frames may be in flight at once.
 *
 * One is the only safe answer against the mask ROM: its sender waits for the
 * bus to go idle before queuing a word, so a master that keeps frames
 * back-to-back starves it and both ends wait forever. Our own payload queues
 * on FIFO space instead and stays ahead, so it can be clocked continuously --
 * which is worth roughly the whole round-trip poll per frame.
 *
 * The cap is the FIFO depth either way: each queued frame returns exactly one
 * word, and a receive overrun would drop a tag the caller is still waiting
 * for, which nothing recovers.
 */
#define SPI_FIFO_DEPTH  8

static u32 rx_depth = 1;

void cetus_set_pipelined(int on)
{
    rx_depth = on ? SPI_FIFO_DEPTH : 1;
}

static int recv_bytes(u8 *out, u32 n)
{
    u32 i = 0, inflight = 0, start = timer_ticks();

    while (i < n) {
        /*
         * Never queue more frames than this phase still owes. Clocking past
         * the last word would consume the start of whatever the far end
         * sends next -- the response header, or the ready word of the command
         * after it -- and those bytes are unrecoverable once read.
         */
        while (inflight < rx_depth && inflight < n - i &&
               (read16(SSP_SR) & SSP_SR_TNF)) {
            write16(SSP_DR, 0);
            inflight++;
        }

        while (read16(SSP_SR) & SSP_SR_RNE) {
            u16 w = read16(SSP_DR);

            inflight--;
            if ((u8)(w >> 8) != res_seq)
                continue;

            res_seq = seq_next(res_seq);
            out[i++] = (u8)w;
            start = timer_ticks();
            if (i == n)
                break;
        }

        if (timer_ticks() - start > WORD_TIMEOUT_TICKS)
            return -1;
    }

    return 0;
}

static void frame_init(u8 *f, u8 op, u8 unit)
{
    u32 i;

    for (i = 0; i < CETUS_FRAME; i++)
        f[i] = 0;
    f[0] = op;
    f[1] = unit;
}

static void frame_put32(u8 *f, u32 off, u32 v)
{
    f[off + 0] = (u8)v;
    f[off + 1] = (u8)(v >> 8);
    f[off + 2] = (u8)(v >> 16);
    f[off + 3] = (u8)(v >> 24);
}

static u32 frame_get32(const u8 *f, u32 off)
{
    return (u32)f[off] | ((u32)f[off + 1] << 8) |
           ((u32)f[off + 2] << 16) | ((u32)f[off + 3] << 24);
}

static int send_command(const u8 *f, u8 *res)
{
    /*
     * Start from a quiet receive FIFO. A command that gave up part way leaves
     * its replies behind, and reading those first costs the whole of the next
     * command before the two ends line up again.
     */
    rx_drain();

    if (recv_rdy() != 0)
        return CETUS_E_RDY;
    if (spi_send_frame(f) != 0)
        return CETUS_E_WRITE;
    if (recv_ack() != 0)
        return CETUS_E_ACK;
    if (recv_bytes(res, CETUS_FRAME) != 0)
        return CETUS_E_RES;
    if (res[0] != CETUS_OK)
        return -(int)(0x100 | res[0]);
    return 0;
}

static u8 unit_code(u32 width)
{
    switch (width) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    }
    return 0xFF;
}

int cetus_status(void)
{
    u8 f[CETUS_FRAME], res[CETUS_FRAME];

    cetus_spi_init();
    frame_init(f, CMD_STATUS, 0);
    return send_command(f, res);
}

int cetus_read(u32 addr, u8 *buf, u32 len, u32 width)
{
    u8 f[CETUS_FRAME], res[CETUS_FRAME];
    u8 unit = unit_code(width);
    int rc;

    if (unit == 0xFF || len == 0 || len % width || addr % width)
        return CETUS_E_ARG;

    cetus_spi_init();
    frame_init(f, CMD_UPLOAD, unit);
    frame_put32(f, 4, len);
    frame_put32(f, 8, addr);
    frame_put32(f, 12, len / width);

    rc = send_command(f, res);
    if (rc != 0)
        return rc;
    if (frame_get32(res, 4) != len)
        return CETUS_E_SHORT;
    if (recv_bytes(buf, len) != 0)
        return CETUS_E_PAY;
    return 0;
}

int cetus_write(u32 addr, const u8 *buf, u32 len, u32 width)
{
    u8 f[CETUS_FRAME], res[CETUS_FRAME];
    u8 unit = unit_code(width);
    u32 off;

    if (unit == 0xFF || len == 0 || len % width || addr % width)
        return CETUS_E_ARG;

    cetus_spi_init();
    frame_init(f, CMD_DOWNLOAD, unit);
    frame_put32(f, 4, len);
    frame_put32(f, 8, addr);
    frame_put32(f, 12, len / width);

    if (recv_rdy() != 0)
        return CETUS_E_RDY;
    if (spi_send_frame(f) != 0)
        return CETUS_E_WRITE;
    if (recv_ack() != 0)
        return CETUS_E_ACK;

    for (off = 0; off < len; off += CETUS_FRAME) {
        u8 chunk[CETUS_FRAME];
        u32 i;

        for (i = 0; i < CETUS_FRAME; i++)
            chunk[i] = (off + i < len) ? buf[off + i] : 0;

        if (spi_send_frame(chunk) != 0)
            return CETUS_E_WRITE;
        if (recv_ack() != 0)
            return CETUS_E_ACK;
    }

    if (recv_bytes(res, CETUS_FRAME) != 0)
        return CETUS_E_RES;
    if (res[0] != CETUS_OK)
        return -(int)(0x100 | res[0]);
    return 0;
}

int cetus_entry(u32 addr)
{
    u8 f[CETUS_FRAME], res[CETUS_FRAME];

    if (addr & 3)
        return CETUS_E_ARG;

    cetus_spi_init();
    frame_init(f, CMD_ENTRY, 0);
    frame_put32(f, 8, addr);

    return send_command(f, res);
}

int cetus_nor_read(u32 offset, u8 *buf, u32 len)
{
    u8 f[CETUS_FRAME], res[CETUS_FRAME];
    int rc;

    if (len == 0 || (len & 3) || (offset & 3))
        return CETUS_E_ARG;

    cetus_spi_init();
    frame_init(f, CMD_NOR_READ, 0);
    frame_put32(f, 4, len);
    frame_put32(f, 8, offset);
    frame_put32(f, 12, len);

    rc = send_command(f, res);
    if (rc != 0)
        return rc;
    if (frame_get32(res, 4) != len)
        return CETUS_E_SHORT;
    if (recv_bytes(buf, len) != 0)
        return CETUS_E_PAY;
    return 0;
}

int cetus_nor_cmd(u8 op, u8 dummy, u8 nbytes, u32 addr, int use_addr, u8 *out)
{
    u8 f[CETUS_FRAME], res[CETUS_FRAME];
    int rc;

    if (nbytes == 0 || nbytes > 8)
        return CETUS_E_ARG;

    cetus_spi_init();
    frame_init(f, CMD_NOR_CMD, 0);
    f[1] = op;
    f[2] = dummy;
    f[3] = nbytes;
    frame_put32(f, 8, addr);
    frame_put32(f, 12, use_addr ? 1 : 0);

    rc = send_command(f, res);
    if (rc != 0)
        return rc;
    if (frame_get32(res, 4) != nbytes)
        return CETUS_E_SHORT;
    if (recv_bytes(out, nbytes) != 0)
        return CETUS_E_PAY;
    return 0;
}

int cetus_nor_id(u32 *id)
{
    u8 b[3];
    int rc = cetus_nor_cmd(OP_RDID, 0, sizeof(b), 0, 0, b);

    if (rc != 0)
        return rc;
    *id = (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16);
    return 0;
}

/*
 * The CP payload, linked in by the parent Makefile. objcopy derives these
 * names from the file it converted.
 */
extern const u8 _binary_cetus_payload_bin_start[];
extern const u8 _binary_cetus_payload_bin_end[];

static int payload_state = CETUS_E_ARG;

int cetus_payload_state(void)
{
    return payload_state;
}

int cetus_payload_start(void)
{
    u32 len = (u32)(_binary_cetus_payload_bin_end -
                    _binary_cetus_payload_bin_start);
    int rc;

    if (len == 0 || (len & 3))
        return CETUS_E_ARG;

    rc = cetus_write(CETUS_PAYLOAD_BASE, _binary_cetus_payload_bin_start,
                     len, 4);
    if (rc != 0)
        return rc;

    rc = cetus_entry(CETUS_PAYLOAD_BASE);
    if (rc != 0)
        return rc;

    /* The payload re-arms the link itself; give it time to start serving
     * before the next command goes out. */
    mdelay(20);

    cetus_set_pipelined(1);
    rc = cetus_status();
    if (rc != 0)
        cetus_set_pipelined(0);
    return rc;
}

int cetus_bring_up(void)
{
    cetus_monitor_boot();
    payload_state = cetus_payload_start();
    return payload_state;
}
