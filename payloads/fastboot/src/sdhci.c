#include "sdhci.h"
#include "timer.h"
#include "io.h"

/*
 * eMMC driver. See sdhci.h for why the controller is not re-initialised and
 * why transfers are PIO.
 */

u16 mmc_last_error;
u32 mmc_last_r1;
u32 mmc_ocr, mmc_rca, mmc_init_step;
int mmc_ready;

#define CMD_TIMEOUT_TICKS   (TIMER0_HZ / 2)     /* 500 ms */
#define BUSY_TIMEOUT_TICKS  (TIMER0_HZ)         /* 1 s    */

/*
 * Reset the CMD and/or DAT line state machines. Self-clearing: the controller
 * drops the bit when the reset completes.
 */
static void sdhci_reset(u8 mask)
{
    u32 start = timer_ticks();

    write8(SDHCI_SOFTWARE_RESET, mask);
    while (read8(SDHCI_SOFTWARE_RESET) & mask) {
        if (timer_ticks() - start > CMD_TIMEOUT_TICKS)
            return;
    }
}

/* Clear a latched failure so the next command starts from a clean host. */
static void sdhci_recover(void)
{
    sdhci_reset(SDHCI_RESET_CMD | SDHCI_RESET_DATA);
    write16(SDHCI_INT_STATUS, read16(SDHCI_INT_STATUS));
    write16(SDHCI_ERR_INT_STATUS, read16(SDHCI_ERR_INT_STATUS));
}

static int wait_inhibit(u32 mask)
{
    u32 start = timer_ticks();

    while (read32(SDHCI_PRESENT_STATE) & mask) {
        if (timer_ticks() - start > CMD_TIMEOUT_TICKS)
            return MMC_ERR_INHIBIT;
    }
    return MMC_OK;
}

/*
 * Wait for one of `mask`'s bits in Normal Interrupt Status, and consume it.
 * An Error Interrupt aborts the wait: the specific cause is latched in
 * mmc_last_error so a failure can be reported rather than just timing out.
 */
static int wait_int(u16 mask, u32 timeout, int err)
{
    u32 start = timer_ticks();
    u16 sts;

    for (;;) {
        sts = read16(SDHCI_INT_STATUS);

        if (sts & SDHCI_INT_ERROR) {
            mmc_last_error = read16(SDHCI_ERR_INT_STATUS);
            write16(SDHCI_ERR_INT_STATUS, mmc_last_error);  /* W1C */
            write16(SDHCI_INT_STATUS, sts);
            sdhci_recover();
            return MMC_ERR_CMD_ERROR;
        }
        if (sts & mask) {
            write16(SDHCI_INT_STATUS, mask);                /* W1C */
            return MMC_OK;
        }
        if (timer_ticks() - start > timeout) {
            sdhci_recover();
            return err;
        }
    }
}

/*
 * Issue one command. `data_blocks` of 0 means no data phase; the caller has
 * already programmed Block Size/Count and Transfer Mode when there is one.
 */
static int send_cmd(u8 index, u32 arg, u16 flags, u32 data_blocks)
{
    int ret;

    ret = wait_inhibit(data_blocks || (flags & 0x3) == SDHCI_CMD_RESP_BUSY
                       ? (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT)
                       : SDHCI_CMD_INHIBIT);
    if (ret) {
        /* Stuck from a previous failure -- clear it and try once more. */
        sdhci_recover();
        ret = wait_inhibit(SDHCI_CMD_INHIBIT);
        if (ret)
            return ret;
    }

    /* Clear any stale status before starting. */
    write16(SDHCI_INT_STATUS, read16(SDHCI_INT_STATUS));
    write16(SDHCI_ERR_INT_STATUS, read16(SDHCI_ERR_INT_STATUS));
    mmc_last_error = 0;

    write32(SDHCI_ARGUMENT, arg);
    dsb();

    /* 16-bit write: a 32-bit store here would also hit Transfer Mode. */
    write16(SDHCI_COMMAND, SDHCI_MAKE_CMD(index, flags));
    dsb();

    return wait_int(SDHCI_INT_CMD_COMPLETE, CMD_TIMEOUT_TICKS,
                    MMC_ERR_CMD_TIMEOUT);
}

/*
 * Set the SDCLK divider. `div` is the SDHCI divider N: the card clock is
 * base/(2*N), except N=0 which means base/1.
 *
 * The SD clock must be stopped while the divider changes, and Internal Clock
 * Stable has to be observed before re-enabling it -- driving the card from an
 * unstable clock is exactly the sort of thing that produces intermittent
 * command timeouts.
 */
static void sdhci_set_clock(u32 div)
{
    u32 start;
    u16 clk;

    clk = read16(SDHCI_CLOCK_CONTROL);
    write16(SDHCI_CLOCK_CONTROL, clk & (u16)~SDHCI_CLK_SD_EN);

    clk = (u16)(((div & 0xff) << SDHCI_CLK_DIV_SHIFT) |
                (((div >> 8) & 0x3) << SDHCI_CLK_DIV_HI_SHIFT) |
                SDHCI_CLK_INT_EN);
    write16(SDHCI_CLOCK_CONTROL, clk);

    start = timer_ticks();
    while (!(read16(SDHCI_CLOCK_CONTROL) & SDHCI_CLK_INT_STABLE)) {
        if (timer_ticks() - start > CMD_TIMEOUT_TICKS)
            break;
    }

    write16(SDHCI_CLOCK_CONTROL, (u16)(clk | SDHCI_CLK_SD_EN));
    udelay(100);
}

/* CMD6 SWITCH, write-byte form: EXT_CSD[index] = value. */
static int mmc_switch(u8 index, u8 value)
{
    u32 arg = (3u << 24) | ((u32)index << 16) | ((u32)value << 8);

    return mmc_raw_cmd(MMC_CMD_SWITCH, arg,
                       SDHCI_CMD_RESP_BUSY | SDHCI_CMD_CRC_CHECK |
                       SDHCI_CMD_INDEX_CHECK, 0);
}

int mmc_init(void)
{
    u16 saved_clk = read16(SDHCI_CLOCK_CONTROL);
    u8  saved_host = read8(SDHCI_HOST_CONTROL);
    u16 r1 = SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK;
    u32 base_khz, div, start, resp;
    int ret;

    mmc_ocr = mmc_rca = 0;
    mmc_init_step = 0;
    mmc_ready = 0;

    /* Capabilities[15:8] is the base clock in MHz. Aim for <= 400 kHz. */
    base_khz = ((read32(SDHCI_CAPABILITIES) >> 8) & 0xff) * 1000;
    if (base_khz == 0)
        base_khz = 48000;
    div = base_khz / (2 * 400);
    if (div == 0)
        div = 1;

    sdhci_reset(SDHCI_RESET_CMD | SDHCI_RESET_DATA);
    write8(SDHCI_HOST_CONTROL, (u8)(saved_host & ~0x22));   /* 1-bit bus */
    sdhci_set_clock(div);

    /* CMD0 GO_IDLE_STATE -- no response. */
    mmc_init_step = 1;
    ret = send_cmd(0, 0, SDHCI_CMD_RESP_NONE, 0);
    if (ret)
        goto out;
    mdelay(2);

    /*
     * CMD1 SEND_OP_COND. Bit 30 requests sector addressing; poll until the
     * card clears its busy bit (31) to say power-up is done.
     */
    mmc_init_step = 2;
    start = timer_ticks();
    for (;;) {
        ret = mmc_raw_cmd(1, 0x40FF8080, SDHCI_CMD_RESP_SHORT, &resp);
        if (ret)
            goto out;
        mmc_ocr = resp;
        if (resp & 0x80000000u)
            break;
        if (timer_ticks() - start > TIMER0_HZ) {   /* 1 s */
            ret = MMC_ERR_CMD_TIMEOUT;
            goto out;
        }
        mdelay(5);
    }

    /* CMD2 ALL_SEND_CID -- 136-bit response, no index check. */
    mmc_init_step = 3;
    ret = mmc_raw_cmd(2, 0, SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC_CHECK, 0);
    if (ret)
        goto out;

    /* CMD3 SET_RELATIVE_ADDR -- the host picks the address for MMC. */
    mmc_init_step = 4;
    mmc_rca = 1;
    ret = mmc_raw_cmd(3, mmc_rca << 16, r1, 0);
    if (ret)
        goto out;

    /* CMD7 SELECT_CARD -- moves the card to transfer state. Still slow. */
    mmc_init_step = 5;
    ret = mmc_raw_cmd(7, mmc_rca << 16,
                      SDHCI_CMD_RESP_BUSY | SDHCI_CMD_CRC_CHECK |
                      SDHCI_CMD_INDEX_CHECK, 0);
    if (ret)
        goto out;

    /*
     * A freshly identified card is in 1-bit legacy mode, whatever the
     * controller was left set to. The ROM leaves Host Control at 8-bit high
     * speed, so reading now would clock 8 lines against a card driving one --
     * which shows up as Data CRC / End Bit errors rather than as silence.
     *
     * CMD6 travels on the CMD line, so these switches work while still in
     * 1-bit mode.
     */
    mmc_init_step = 6;
    ret = mmc_switch(EXT_CSD_BUS_WIDTH, MMC_BUS_WIDTH_8);
    if (ret)
        goto out;

    mmc_init_step = 7;
    ret = mmc_switch(EXT_CSD_HS_TIMING, 1);
    if (ret)
        goto out;

    /*
     * Enable Buffer Read Ready and Buffer Write Ready.
     *
     * Normal Interrupt STATUS ENABLE gates whether a bit ever appears in the
     * status register -- it is not just an IRQ mask. The ROM leaves it at
     * 0x400B (command/transfer complete, DMA, error), which is all its
     * DMA-based reader needs, so bits 4 and 5 never set and a PIO transfer
     * waits for a flag that can never arrive. That presents as a data timeout
     * with no error bits at all.
     */
    write16(SDHCI_INT_ENABLE,
            read16(SDHCI_INT_ENABLE) | SDHCI_INT_BUF_READ_RDY |
            SDHCI_INT_BUF_WRITE_RDY);
    write16(SDHCI_ERR_INT_ENABLE, 0x03ff);

    /* Card and controller now agree: 8-bit, high speed. */
    write8(SDHCI_HOST_CONTROL, saved_host);
    write16(SDHCI_CLOCK_CONTROL, saved_clk);
    udelay(100);

    mmc_init_step = 0;
    mmc_ready = 1;
    return MMC_OK;

out:
    write8(SDHCI_HOST_CONTROL, saved_host);
    write16(SDHCI_CLOCK_CONTROL, saved_clk);
    udelay(100);
    return ret;
}

int mmc_raw_cmd(u8 index, u32 arg, u16 flags, u32 *resp)
{
    int ret = send_cmd(index, arg, flags, 0);

    if (resp)
        *resp = read32(SDHCI_RESPONSE);

    /* An R1b command holds DAT0 low until it finishes; consume that. */
    if (ret == MMC_OK && (flags & 0x3) == SDHCI_CMD_RESP_BUSY)
        ret = wait_int(SDHCI_INT_XFER_COMPLETE, BUSY_TIMEOUT_TICKS,
                       MMC_ERR_XFER);
    return ret;
}

/*
 * A failed command usually means the card is no longer where we think it is --
 * knocked out of transfer state by a stray CMD0, a power blip, or a reset. Mark
 * it not-ready so the next operation re-identifies instead of repeating the
 * same failure against stale state.
 */
static int mmc_fail(int ret)
{
    if (ret != MMC_OK)
        mmc_ready = 0;
    return ret;
}

int mmc_select_partition(u32 access)
{
    u32 arg;
    int ret;
    u16 flags = SDHCI_CMD_RESP_BUSY | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_INDEX_CHECK;

    int fret;

    access &= PARTITION_ACCESS_MASK;

    /* Clear PARTITION_ACCESS, then set the value we want. */
    arg = (2u << 24) | (EXT_CSD_PARTITION_CONFIG << 16) |
          (PARTITION_ACCESS_MASK << 8);
    ret = send_cmd(MMC_CMD_SWITCH, arg, flags, 0);
    if (ret)
        return mmc_fail(ret);
    /* R1b: the card holds DAT0 low while it applies the change. */
    ret = wait_int(SDHCI_INT_XFER_COMPLETE, BUSY_TIMEOUT_TICKS, MMC_ERR_XFER);
    if (ret)
        return mmc_fail(ret);

    if (access == 0)
        return MMC_OK;      /* clearing already selected the user area */

    arg = (1u << 24) | (EXT_CSD_PARTITION_CONFIG << 16) | (access << 8);
    ret = send_cmd(MMC_CMD_SWITCH, arg, flags, 0);
    if (ret)
        return mmc_fail(ret);

    fret = wait_int(SDHCI_INT_XFER_COMPLETE, BUSY_TIMEOUT_TICKS, MMC_ERR_XFER);
    return mmc_fail(fret);
}

int mmc_read_blocks(u32 start_block, void *buf, u32 nblocks)
{
    u32 *out = (u32 *)buf;
    u16 mode, flags;
    u32 b, w;
    int ret, multi = (nblocks > 1);

    if (nblocks == 0)
        return MMC_OK;

    ret = wait_inhibit(SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT);
    if (ret)
        return mmc_fail(ret);

    write16(SDHCI_BLOCK_SIZE, 512);
    write16(SDHCI_BLOCK_COUNT, (u16)nblocks);

    /*
     * PIO, so DMA Enable stays clear. Auto CMD12 saves issuing
     * STOP_TRANSMISSION by hand after a multi-block read.
     */
    mode = SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN;
    if (multi)
        mode |= SDHCI_TRNS_MULTI | SDHCI_TRNS_AUTO_CMD12;
    write16(SDHCI_TRANSFER_MODE, mode);
    dsb();

    flags = SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC_CHECK |
            SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_DATA_PRESENT;

    /*
     * The argument is a block address, not a byte offset: this part is well
     * over 2 GB so it is certainly high-capacity addressing.
     */
    ret = send_cmd(multi ? MMC_CMD_READ_MULTIPLE_BLOCK
                         : MMC_CMD_READ_SINGLE_BLOCK,
                   start_block, flags, nblocks);
    if (ret)
        return mmc_fail(ret);

    for (b = 0; b < nblocks; b++) {
        ret = wait_int(SDHCI_INT_BUF_READ_RDY, CMD_TIMEOUT_TICKS,
                       MMC_ERR_DATA_TIMEOUT);
        if (ret)
            return mmc_fail(ret);

        for (w = 0; w < 512 / 4; w++)
            *out++ = read32(SDHCI_BUFFER);
    }

    return mmc_fail(wait_int(SDHCI_INT_XFER_COMPLETE, CMD_TIMEOUT_TICKS,
                             MMC_ERR_XFER));
}

int mmc_wait_ready(void)
{
    u32 start = timer_ticks();
    u32 resp = 0;
    int ret;

    for (;;) {
        ret = mmc_raw_cmd(MMC_CMD_SEND_STATUS, mmc_rca << 16,
                          SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC_CHECK |
                          SDHCI_CMD_INDEX_CHECK, &resp);
        if (ret)
            return ret;

        mmc_last_r1 = resp;
        if (resp & MMC_R1_ERROR_MASK)
            return MMC_ERR_CARD_STATUS;

        if ((resp & MMC_R1_READY_FOR_DATA) &&
            MMC_R1_STATE(resp) == MMC_STATE_TRAN)
            return MMC_OK;

        /*
         * Programming a block is slow and erase-block-sized internally, so a
         * write can sit in the prg state for a long time. The generous budget
         * costs nothing when the card is quick.
         */
        if (timer_ticks() - start > BUSY_TIMEOUT_TICKS * 5)
            return MMC_ERR_XFER;
    }
}

/*
 * Mirror of mmc_read_blocks(). Two differences matter beyond the direction bit:
 *
 *  - Transfer Complete only means the last block reached the card, not that it
 *    was programmed, so this ends by polling CMD13 via mmc_wait_ready(). That
 *    is also the only place a write-protect violation can be observed.
 *  - An abort mid-transfer leaves the card in the receive state with the
 *    controller's DAT line reset underneath it, so the card is stopped with
 *    CMD12 before giving up. The read path gets away without this because
 *    mmc_fail() forces a re-identification either way, but leaving a card
 *    holding DAT0 low makes even CMD0 unreliable.
 */
int mmc_write_blocks(u32 start_block, const void *buf, u32 nblocks)
{
    const u32 *in = (const u32 *)buf;
    u16 mode, flags;
    u32 b, w;
    int ret, multi = (nblocks > 1);

    if (nblocks == 0)
        return MMC_OK;

    ret = wait_inhibit(SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT);
    if (ret)
        return mmc_fail(ret);

    write16(SDHCI_BLOCK_SIZE, 512);
    write16(SDHCI_BLOCK_COUNT, (u16)nblocks);

    /* Direction is the absence of SDHCI_TRNS_READ. */
    mode = SDHCI_TRNS_BLK_CNT_EN;
    if (multi)
        mode |= SDHCI_TRNS_MULTI | SDHCI_TRNS_AUTO_CMD12;
    write16(SDHCI_TRANSFER_MODE, mode);
    dsb();

    flags = SDHCI_CMD_RESP_SHORT | SDHCI_CMD_CRC_CHECK |
            SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_DATA_PRESENT;

    ret = send_cmd(multi ? MMC_CMD_WRITE_MULTIPLE_BLOCK : MMC_CMD_WRITE_BLOCK,
                   start_block, flags, nblocks);
    if (ret)
        return mmc_fail(ret);

    for (b = 0; b < nblocks; b++) {
        ret = wait_int(SDHCI_INT_BUF_WRITE_RDY, CMD_TIMEOUT_TICKS,
                       MMC_ERR_WRITE_TIMEOUT);
        if (ret)
            goto abort;

        for (w = 0; w < 512 / 4; w++)
            write32(SDHCI_BUFFER, *in++);
    }

    /*
     * Busy timeout, not the command timeout: the card may hold DAT0 low
     * through the final block's programming before Transfer Complete.
     */
    ret = wait_int(SDHCI_INT_XFER_COMPLETE, BUSY_TIMEOUT_TICKS, MMC_ERR_XFER);
    if (ret)
        goto abort;

    return mmc_fail(mmc_wait_ready());

abort:
    /* mmc_raw_cmd() rather than send_cmd(): CMD12 is R1b, and leaving the
     * card's busy response unconsumed is how the next command inherits the
     * failure. */
    if (multi)
        mmc_raw_cmd(MMC_CMD_STOP_TRANSMISSION, 0,
                    SDHCI_CMD_RESP_BUSY | SDHCI_CMD_CRC_CHECK |
                    SDHCI_CMD_INDEX_CHECK, 0);
    return mmc_fail(ret);
}
