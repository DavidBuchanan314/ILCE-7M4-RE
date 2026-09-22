#ifndef FASTBOOT_SDHCI_H
#define FASTBOOT_SDHCI_H

#include "io.h"

/*
 * eMMC via the Arasan SD Host Controller.
 * arch/arm64/boot/dts/cxd/cxd90057.dtsi:768 declares
 * `emmc0 { compatible = "arasan,sdhci"; }`; Host Controller Version (0xFE)
 * reads 0x1004, spec 4.00.
 *
 * The controller is left as the ROM set it up -- bus powered, 8-bit, high
 * speed, clock stable -- so this only has to issue commands. Transfers are
 * PIO, which sidesteps DMA coherency entirely.
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

/* Card identification must run at <= 400 kHz; the ROM leaves the controller
 * clocked for data transfer. */
#define SDHCI_CLK_INT_EN        BIT(0)
#define SDHCI_CLK_INT_STABLE    BIT(1)
#define SDHCI_CLK_SD_EN         BIT(2)
#define SDHCI_CLK_DIV_SHIFT     8
#define SDHCI_CLK_DIV_HI_SHIFT  6

/* The controller latches a failure and refuses further commands until CMD+DAT
 * are reset. */
#define SDHCI_RESET_ALL         BIT(0)
#define SDHCI_RESET_CMD         BIT(1)
#define SDHCI_RESET_DATA        BIT(2)

/* Present State */
#define SDHCI_CMD_INHIBIT       BIT(0)
#define SDHCI_DATA_INHIBIT      BIT(1)

/* Normal Interrupt Status */
#define SDHCI_INT_CMD_COMPLETE  BIT(0)
#define SDHCI_INT_XFER_COMPLETE BIT(1)
#define SDHCI_INT_BUF_WRITE_RDY BIT(4)
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
#define MMC_CMD_SWITCH               6
#define MMC_CMD_STOP_TRANSMISSION   12
#define MMC_CMD_SEND_STATUS         13
#define MMC_CMD_READ_SINGLE_BLOCK   17
#define MMC_CMD_READ_MULTIPLE_BLOCK 18
#define MMC_CMD_WRITE_BLOCK         24
#define MMC_CMD_WRITE_MULTIPLE_BLOCK 25

/*
 * R1 card status. Transfer Complete only means the last block reached the bus;
 * a programming failure -- write-protected partition, out-of-range address --
 * surfaces only in the NEXT status response, so the write path polls CMD13.
 *
 * ERROR_MASK covers every sticky error bit: 31..26 address and erase faults
 * plus WP_VIOLATION, 24 lock/unlock, 23..19 CRC/illegal/ECC/CC/internal,
 * 16 CID-CSD overwrite, 15 WP erase skip, 7 switch error.
 */
#define MMC_R1_ERROR_MASK           0xFDF98080u
#define MMC_R1_WP_VIOLATION         BIT(26)
#define MMC_R1_READY_FOR_DATA       BIT(8)
#define MMC_R1_STATE(r)             (((r) >> 9) & 0xf)
#define MMC_STATE_TRAN              4

/*
 * EXT_CSD[179] PARTITION_CONFIG, low 3 bits = PARTITION_ACCESS:
 *   0 user area, 1 boot0, 2 boot1, 3 RPMB, 4..7 GP1..GP4
 *
 * CMD6 argument is (ACCESS_MODE << 24) | (INDEX << 16) | (VALUE << 8), with
 * ACCESS_MODE 1 = set bits, 2 = clear bits, 3 = write byte. PARTITION_CONFIG
 * also holds BOOT_PARTITION_ENABLE and BOOT_ACK, so the field is cleared and
 * set rather than written as a byte.
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
    MMC_ERR_WRITE_TIMEOUT= -6,  /* no Buffer Write Ready        */
    MMC_ERR_CARD_STATUS = -7,   /* card reported an R1 error    */
    MMC_ERR_RANGE       = -8,   /* write would leave the target */
};

/* Last Error Interrupt Status seen, for diagnostics. */
extern u16 mmc_last_error;

/* Last R1 card status seen by mmc_wait_ready(), for diagnostics. */
extern u32 mmc_last_r1;

/* One raw command, with the 48-bit response. */
int mmc_raw_cmd(u8 index, u32 arg, u16 flags, u32 *resp);

/*
 * Full identification: CMD0 -> CMD1 (poll) -> CMD2 -> CMD3 -> CMD7 at
 * ~400 kHz, then back to the original clock. The ROM is diverted before it
 * identifies the card, so the payload does not inherit one.
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

/* Write `nblocks` 512-byte blocks from `buf` starting at `start_block`. */
int mmc_write_blocks(u32 start_block, const void *buf, u32 nblocks);

/*
 * Poll CMD13 until the card leaves the programming state and reports
 * READY_FOR_DATA. Returns MMC_ERR_CARD_STATUS if the status word carries any
 * error bit; the word itself is left in mmc_last_r1.
 */
int mmc_wait_ready(void);

#endif /* FASTBOOT_SDHCI_H */
