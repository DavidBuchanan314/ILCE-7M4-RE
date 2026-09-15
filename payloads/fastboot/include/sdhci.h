#ifndef FASTBOOT_SDHCI_H
#define FASTBOOT_SDHCI_H

#include "io.h"

/*
 * eMMC via the Arasan SD Host Controller.
 *
 * cxd90057.dtsi declares `emmc0 { compatible = "arasan,sdhci"; }`, so this is
 * a standard SDHCI part rather than a vendor design, and BOOT.md places the
 * host at 0xF10D6000. Both were confirmed by reading the live controller:
 * Host Controller Version (0xFE) = 0x1004, i.e. spec 4.00.
 *
 * We do NOT initialise the controller. By the time a serial-boot payload runs
 * the ROM has already brought the eMMC up, which a register dump confirms:
 *
 *   0x24 Present State  01ff00f0  card inserted, CMD/DAT idle
 *   0x28 Host Control   24        8-bit bus, high speed
 *   0x29 Power Control  0f        bus power on, 3.3 V
 *   0x2C Clock Control  0007      internal clock stable, SD clock enabled
 *
 * So this driver only has to issue commands. Transfers use PIO rather than
 * SDMA/ADMA: the ROM's own reader uses 0xFE030008 as scratch, which lands
 * inside our .usbdma arena, and PIO avoids both that collision and any
 * question about DMA coherency.
 */

#define SDHCI_BASE              0xF10D6000ull

#define SDHCI_DMA_ADDRESS       (SDHCI_BASE + 0x00)
#define SDHCI_BLOCK_SIZE        (SDHCI_BASE + 0x04)   /* 16-bit */
#define SDHCI_BLOCK_COUNT       (SDHCI_BASE + 0x06)   /* 16-bit */
#define SDHCI_ARGUMENT          (SDHCI_BASE + 0x08)
#define SDHCI_TRANSFER_MODE     (SDHCI_BASE + 0x0C)   /* 16-bit */
#define SDHCI_COMMAND           (SDHCI_BASE + 0x0E)   /* 16-bit */
#define SDHCI_RESPONSE          (SDHCI_BASE + 0x10)
#define SDHCI_BUFFER            (SDHCI_BASE + 0x20)
#define SDHCI_PRESENT_STATE     (SDHCI_BASE + 0x24)
#define SDHCI_HOST_CONTROL      (SDHCI_BASE + 0x28)   /* 8-bit  */
#define SDHCI_INT_STATUS        (SDHCI_BASE + 0x30)   /* 16-bit */
#define SDHCI_INT_ENABLE        (SDHCI_BASE + 0x34)   /* 16-bit */
#define SDHCI_ERR_INT_ENABLE    (SDHCI_BASE + 0x36)   /* 16-bit */
#define SDHCI_ERR_INT_STATUS    (SDHCI_BASE + 0x32)   /* 16-bit */
#define SDHCI_CLOCK_CONTROL     (SDHCI_BASE + 0x2C)   /* 16-bit */
#define SDHCI_SOFTWARE_RESET    (SDHCI_BASE + 0x2F)   /* 8-bit  */
#define SDHCI_CAPABILITIES      (SDHCI_BASE + 0x40)
#define SDHCI_HOST_VERSION      (SDHCI_BASE + 0xFE)   /* 16-bit */

/* Clock Control. Card identification must run at <= 400 kHz; the controller is
 * left clocked for data transfer by the ROM, which is why CMD2/CMD3 time out
 * until the divider is applied. */
#define SDHCI_CLK_INT_EN        BIT(0)
#define SDHCI_CLK_INT_STABLE    BIT(1)
#define SDHCI_CLK_SD_EN         BIT(2)
#define SDHCI_CLK_DIV_SHIFT     8
#define SDHCI_CLK_DIV_HI_SHIFT  6

/* Software Reset. Resetting CMD+DAT after an error is mandatory: the
 * controller latches the failure and refuses further commands until the lines
 * are reset, so without it one bad command wedges the host for good. */
#define SDHCI_RESET_ALL         BIT(0)
#define SDHCI_RESET_CMD         BIT(1)
#define SDHCI_RESET_DATA        BIT(2)

/* Present State */
#define SDHCI_CMD_INHIBIT       BIT(0)
#define SDHCI_DATA_INHIBIT      BIT(1)

/* Normal Interrupt Status */
#define SDHCI_INT_CMD_COMPLETE  BIT(0)
#define SDHCI_INT_XFER_COMPLETE BIT(1)
#define SDHCI_INT_BUF_READ_RDY  BIT(5)
#define SDHCI_INT_ERROR         BIT(15)

/* Transfer Mode */
#define SDHCI_TRNS_DMA          BIT(0)
#define SDHCI_TRNS_BLK_CNT_EN   BIT(1)
#define SDHCI_TRNS_AUTO_CMD12   BIT(2)
#define SDHCI_TRNS_READ         BIT(4)
#define SDHCI_TRNS_MULTI        BIT(5)

/* Command register: response type and checks */
#define SDHCI_CMD_RESP_NONE     0x00
#define SDHCI_CMD_RESP_LONG     0x01    /* 136-bit */
#define SDHCI_CMD_RESP_SHORT    0x02    /* 48-bit  */
#define SDHCI_CMD_RESP_BUSY     0x03    /* 48-bit with busy */
#define SDHCI_CMD_CRC_CHECK     BIT(3)
#define SDHCI_CMD_INDEX_CHECK   BIT(4)
#define SDHCI_CMD_DATA_PRESENT  BIT(5)
#define SDHCI_MAKE_CMD(idx, f)  (u16)(((idx) << 8) | (f))

/* MMC command indices */
#define MMC_CMD_SWITCH              6
#define MMC_CMD_READ_SINGLE_BLOCK   17
#define MMC_CMD_READ_MULTIPLE_BLOCK 18

/*
 * EXT_CSD[179] PARTITION_CONFIG, low 3 bits = PARTITION_ACCESS:
 *   0 user area, 1 boot0, 2 boot1, 3 RPMB, 4..7 GP1..GP4
 *
 * The CMD6 argument is (ACCESS_MODE << 24) | (INDEX << 16) | (VALUE << 8),
 * with ACCESS_MODE 1 = set bits, 2 = clear bits, 3 = write byte. We clear the
 * field then set it rather than writing the whole byte, because
 * PARTITION_CONFIG also holds BOOT_PARTITION_ENABLE and BOOT_ACK, which a
 * blind write-byte would destroy. This is exactly what the ROM does -- its
 * observed argument is 0x02B30700.
 */
#define EXT_CSD_BUS_WIDTH           183
#define EXT_CSD_HS_TIMING           185
#define EXT_CSD_PARTITION_CONFIG    179

/* EXT_CSD[183] BUS_WIDTH values. */
#define MMC_BUS_WIDTH_1             0
#define MMC_BUS_WIDTH_4             1
#define MMC_BUS_WIDTH_8             2
#define PARTITION_ACCESS_MASK       0x07

enum mmc_status {
    MMC_OK              =  0,
    MMC_ERR_INHIBIT     = -1,   /* bus never went idle          */
    MMC_ERR_CMD_TIMEOUT = -2,   /* no Command Complete          */
    MMC_ERR_CMD_ERROR   = -3,   /* controller raised Error Int  */
    MMC_ERR_DATA_TIMEOUT= -4,   /* no Buffer Read Ready         */
    MMC_ERR_XFER        = -5,   /* no Transfer Complete         */
};

/* Last Error Interrupt Status seen, for diagnostics. */
extern u16 mmc_last_error;

/*
 * Issue one raw command and hand back the 48-bit response. Exposed so the
 * card can be probed interactively over fastboot rather than by rebuilding
 * for each experiment.
 */
int mmc_raw_cmd(u8 index, u32 arg, u16 flags, u32 *resp);

/*
 * Full card identification: CMD0 -> CMD1 (poll) -> CMD2 -> CMD3 -> CMD7, run
 * at ~400 kHz and then restored to the original clock.
 *
 * Needed because the payload does NOT inherit an initialised card on this boot
 * path -- the ROM is diverted before identification, and its own "init" is
 * only CMD5 SLEEP_AWAKE against an RCA that was never assigned.
 */
int mmc_init(void);

/* Diagnostics from the last mmc_init(). */
extern u32 mmc_ocr, mmc_rca, mmc_init_step;

/* Non-zero once identification has succeeded. Cleared on every attempt. */
extern int mmc_ready;

/* Select an eMMC hardware partition (PARTITION_ACCESS value). */
int mmc_select_partition(u32 access);

/* Read `nblocks` 512-byte blocks starting at `start_block` into `buf`. */
int mmc_read_blocks(u32 start_block, void *buf, u32 nblocks);

#endif /* FASTBOOT_SDHCI_H */
