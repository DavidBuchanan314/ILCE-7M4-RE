#include "spacc.h"
#include "timer.h"
#include "io.h"

/*
 * See spacc.h for the register map's provenance and the live core readback.
 *
 * One job per 512-byte sector is not a choice: XTS restarts its tweak at every
 * sector, so sectors cannot be batched into one job however contiguous they
 * are on the card.
 */

u32 spacc_last_status;

#define SPACC_CTX_INDEX     0
#define SECTOR_SIZE         512

/* Job timeout. A 512-byte AES job is sub-microsecond; this is only here so a
 * wedged core reports instead of hanging the payload. */
#define SPACC_TIMEOUT_TICKS (TIMER0_HZ / 10)    /* 100 ms */

/*
 * The AES-XTS key the user-area partitions are encrypted with.
 *
 * Derived on the host by scripts/parse_emmc_partitions.py, which indexes the
 * bootrom's key scrambling table at 0xFFFFB800 with an offset table taken from
 * the eMMC loader image. Embedded rather than derived at runtime because that
 * loader is precisely what this payload replaces -- deriving it here would
 * mean decrypting boot0 first, with a different key, to get the offsets.
 *
 * Consequence: this is the key for THIS bootrom and THIS loader version. On
 * another model, or another firmware revision that moves the offset table, it
 * is simply wrong and every sector comes out as noise.
 */
static const u8 part_key[64] = {
    0x16, 0x2c, 0x2a, 0x3b, 0x4b, 0xc1, 0x0d, 0xa3,
    0xff, 0x43, 0x7b, 0xf9, 0x69, 0x9c, 0xe0, 0x95,
    0x7e, 0x4a, 0xec, 0x75, 0x24, 0xdf, 0x1e, 0xa5,
    0xbf, 0xad, 0x74, 0x58, 0xf9, 0x87, 0xbe, 0x01,
    0x16, 0x2c, 0x2a, 0x3b, 0x4b, 0xc1, 0x0d, 0xa3,
    0xff, 0x43, 0x7b, 0xf9, 0x69, 0x9c, 0xe0, 0x95,
    0x17, 0xbe, 0x3b, 0x47, 0xd4, 0x94, 0x2d, 0x32,
    0x5d, 0x2a, 0x47, 0x57, 0xba, 0x91, 0xc3, 0x30,
};

/*
 * Source and destination descriptor tables. A DDT is a zero-terminated list of
 * {32-bit address, 32-bit length} pairs -- one entry each here, since a sector
 * is contiguous. The addresses are physical; with the MMU off that is the same
 * number the CPU uses.
 */
#define DMA_SECTION __attribute__((section(".usbdma"), aligned(64)))

static u32 src_ddt[4] DMA_SECTION;
static u32 dst_ddt[4] DMA_SECTION;

static int spacc_ready;

/* Context pages are 128 bytes (CONFIG[22:20] = 7, page = 1 << 7). */
static inline u64 ctx_page(void)
{
    return SPACC_CTX_CIPH_KEY + (u64)SPACC_CTX_INDEX * 128;
}

/*
 * Context memory is written as 32-bit words, little-endian, matching the
 * driver's pdu_to_dev32_s() on a little-endian host. Byte stores into the
 * context page are not used: it is register space, not memory.
 */
static void ctx_write(u32 off, const u8 *data, u32 len)
{
    u32 i;

    for (i = 0; i < len; i += 4) {
        u32 w = (u32)data[i] | ((u32)data[i + 1] << 8) |
                ((u32)data[i + 2] << 16) | ((u32)data[i + 3] << 24);
        write32(ctx_page() + off + i, w);
    }
}

void spacc_init(void)
{
    /* Key1 and key2 are the two halves of the 64-byte key. */
    ctx_write(SPACC_CTX_XTS_KEY1, part_key, 32);
    ctx_write(SPACC_CTX_XTS_KEY2, part_key + 32, 32);

    /* Interrupts stay masked throughout; every wait here is a poll. */
    write32(SPACC_REG_IRQ_EN, 0);
    dsb();

    spacc_ready = 1;
}

int spacc_xts_sector(void *dst, const void *src, u32 sector, int encrypt)
{
    u32 start, ctrl, sts;
    u8 tweak[16];
    u32 i;

    if (!spacc_ready)
        spacc_init();

    /* The tweak is the absolute sector number, 16 bytes little-endian --
     * the same value scripts/parse_emmc_partitions.py uses. */
    for (i = 0; i < 16; i++)
        tweak[i] = (i < 4) ? (u8)(sector >> (i * 8)) : 0;
    ctx_write(SPACC_CTX_XTS_IV, tweak, 16);

    src_ddt[0] = (u32)(unsigned long)src;
    src_ddt[1] = SECTOR_SIZE;
    src_ddt[2] = 0;
    src_ddt[3] = 0;

    dst_ddt[0] = (u32)(unsigned long)dst;
    dst_ddt[1] = SECTOR_SIZE;
    dst_ddt[2] = 0;
    dst_ddt[3] = 0;
    dma_wmb();

    write32(SPACC_REG_SRC_PTR, (u32)(unsigned long)src_ddt);
    write32(SPACC_REG_DST_PTR, (u32)(unsigned long)dst_ddt);
    write32(SPACC_REG_OFFSET, 0);
    write32(SPACC_REG_PRE_AAD_LEN, 0);
    write32(SPACC_REG_POST_AAD_LEN, 0);
    write32(SPACC_REG_PROC_LEN, SECTOR_SIZE);
    write32(SPACC_REG_ICV_LEN, 0);
    write32(SPACC_REG_ICV_OFFSET, 0);
    write32(SPACC_REG_IV_OFFSET, 0);
    write32(SPACC_REG_AUX_INFO, 0);

    /*
     * KEY_SZ takes half the combined key length for XTS -- 32, not 64 -- with
     * bit 31 marking it as the cipher (rather than hash) key size and the
     * context index in bits 15:8.
     */
    write32(SPACC_REG_KEY_SZ, 32u | BIT(31) | (SPACC_CTX_INDEX << 8));

    write32(SPACC_REG_SW_CTRL, 0);

    ctrl = SPACC_CTRL_CIPH_ALG_AES | SPACC_CTRL_CIPH_MODE_XTS |
           SPACC_CTRL_MSG_BEGIN | SPACC_CTRL_MSG_END |
           ((u32)SPACC_CTX_INDEX << 16);
    if (encrypt)
        ctrl |= SPACC_CTRL_ENCRYPT;

    dsb();
    write32(SPACC_REG_CTRL, ctrl);      /* starts the job */
    dsb();

    /* Wait for the job to reach the status FIFO. */
    start = timer_ticks();
    while (SPACC_FIFO_STAT_CNT(read32(SPACC_REG_FIFO_STAT)) == 0) {
        if (timer_ticks() - start > SPACC_TIMEOUT_TICKS)
            return SPACC_ERR_TIMEOUT;
    }

    /* Pop first, then read STATUS: the pop is what advances the FIFO to the
     * entry STATUS reports on. */
    write32(SPACC_REG_STAT_POP, 1);
    dsb();
    sts = read32(SPACC_REG_STATUS);
    spacc_last_status = sts;

    if (SPACC_STATUS_RET(sts) != 0)
        return SPACC_ERR_RETCODE;

    dsb();
    return SPACC_OK;
}
