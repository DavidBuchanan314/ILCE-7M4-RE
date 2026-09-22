#ifndef FASTBOOT_SPACC_H
#define FASTBOOT_SPACC_H

#include "io.h"

/*
 * Elliptic/Synopsys SPAcc, used for the AES-XTS-256 the user-area partitions
 * are encrypted with. Register map and job sequence come from the vendor's
 * GPL driver, which ships out of tree in elpspacc/ beside the kernel.
 *
 * The core reports ID 0x00010850 -- major 5 -- and CONFIG 0x16710080: 128
 * contexts, cipher page 1 << 7 = 128 B, DMA type 1 = DDT/scattergather
 * (elpspacc/export/elpspacc.h:13, decoded in
 * elpspacc/driver/src/core/elpspacc/spacc_init.c:51).
 *
 * CTRL uses the 4.15+ layout, where HASH_ALG is bit 3 rather than bit 4
 * (elpspacc/export/elpspacchw.h:336 against :320). That is not inferred from
 * the major number: the driver's CTRL_SET_*_GEN helpers take a spacc_device
 * and ignore it, returning the _415 form unconditionally
 * (elpspacc/driver/src/core/elpspacc/spacc_open.c:39).
 */

/* Register offsets: elpspacc/export/elpspacchw.h:43 onwards. */
#define SPACC_BASE              0xF7FC0000ull

#define SPACC_REG_IRQ_EN        (SPACC_BASE + 0x000)
#define SPACC_REG_IRQ_STAT      (SPACC_BASE + 0x004)
#define SPACC_REG_FIFO_STAT     (SPACC_BASE + 0x00C)
#define SPACC_REG_SRC_PTR       (SPACC_BASE + 0x020)
#define SPACC_REG_DST_PTR       (SPACC_BASE + 0x024)
#define SPACC_REG_OFFSET        (SPACC_BASE + 0x028)
#define SPACC_REG_PRE_AAD_LEN   (SPACC_BASE + 0x02C)
#define SPACC_REG_POST_AAD_LEN  (SPACC_BASE + 0x030)
#define SPACC_REG_PROC_LEN      (SPACC_BASE + 0x034)
#define SPACC_REG_ICV_LEN       (SPACC_BASE + 0x038)
#define SPACC_REG_ICV_OFFSET    (SPACC_BASE + 0x03C)
#define SPACC_REG_IV_OFFSET     (SPACC_BASE + 0x040)
#define SPACC_REG_SW_CTRL       (SPACC_BASE + 0x044)
#define SPACC_REG_AUX_INFO      (SPACC_BASE + 0x048)
#define SPACC_REG_CTRL          (SPACC_BASE + 0x04C)
#define SPACC_REG_STAT_POP      (SPACC_BASE + 0x050)
#define SPACC_REG_STATUS        (SPACC_BASE + 0x054)
#define SPACC_REG_KEY_SZ        (SPACC_BASE + 0x100)
#define SPACC_REG_ID            (SPACC_BASE + 0x180)
#define SPACC_REG_CONFIG        (SPACC_BASE + 0x184)
#define SPACC_REG_SECURE_CTRL   (SPACC_BASE + 0x1C0)

/* Cipher context pages start here, one page per context. */
#define SPACC_CTX_CIPH_KEY      (SPACC_BASE + 0x4000)

/*
 * Layout within a cipher context page for XTS, from spacc_write_context()
 * (elpspacc/driver/src/core/elpspacc/spacc_set_context.c:72): key1 at 0, key2
 * at 48, the tweak at 32. KEY_SZ is programmed with HALF the combined key
 * length -- 32 for AES-256-XTS, not 64 -- as at :76.
 */
#define SPACC_CTX_XTS_KEY1      0
#define SPACC_CTX_XTS_IV        32
#define SPACC_CTX_XTS_KEY2      48

#define SPACC_FIFO_STAT_CNT(x)  (((x) >> 16) & 0x1ff)
#define SPACC_STATUS_RET(x)     (((x) >> 24) & 0x7)

/* CTRL fields, 4.15+ layout. */
#define SPACC_CTRL_CIPH_ALG_AES (2u << 0)
#define SPACC_CTRL_CIPH_MODE_XTS (10u << 8)
#define SPACC_CTRL_MSG_BEGIN    BIT(14)
#define SPACC_CTRL_MSG_END      BIT(15)
#define SPACC_CTRL_ENCRYPT      BIT(24)

enum spacc_status {
    SPACC_OK            =  0,
    SPACC_ERR_TIMEOUT   = -1,   /* job never appeared in the status FIFO */
    SPACC_ERR_RETCODE   = -2,   /* core reported a failure return code   */
};

/* Last STATUS word and return code, for diagnostics. */
extern u32 spacc_last_status;

/* Load the partition key into a context. The tweak is rewritten per sector. */
void spacc_init(void);

/*
 * AES-XTS-256 one 512-byte sector in place, with `sector` as the tweak.
 * `encrypt` selects the direction. src and dst may be the same buffer.
 */
int spacc_xts_sector(void *dst, const void *src, u32 sector, int encrypt);

#endif /* FASTBOOT_SPACC_H */
