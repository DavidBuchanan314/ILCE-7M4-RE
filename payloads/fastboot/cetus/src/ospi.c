#include "io.h"
#include "ospi.h"
#include "mbox.h"

#define OSPI_CFG            (OSPI_BASE + 0x00)
#define OSPI_DEVRD          (OSPI_BASE + 0x04)
#define OSPI_DEVWR          (OSPI_BASE + 0x08)
#define OSPI_DELAY          (OSPI_BASE + 0x0C)
#define OSPI_RDDATACAP      (OSPI_BASE + 0x10)
#define OSPI_DEVSZ          (OSPI_BASE + 0x14)
#define OSPI_SRAMPART       (OSPI_BASE + 0x18)
#define OSPI_INDAHBTRIG     (OSPI_BASE + 0x1C)
#define OSPI_REMAPADDR      (OSPI_BASE + 0x24)
#define OSPI_SRAMFILL       (OSPI_BASE + 0x2C)
#define OSPI_IRQSTAT        (OSPI_BASE + 0x40)
#define OSPI_INDRD          (OSPI_BASE + 0x60)
#define OSPI_INDRDSTADDR    (OSPI_BASE + 0x68)
#define OSPI_INDRDCNT       (OSPI_BASE + 0x6C)
#define OSPI_FLASHCMD       (OSPI_BASE + 0x90)
#define OSPI_FLASHCMDADDR   (OSPI_BASE + 0x94)
#define OSPI_FLASHRDDATALO  (OSPI_BASE + 0xA0)
#define OSPI_FLASHRDDATAUP  (OSPI_BASE + 0xA4)

/*
 * The controller's I/O mode is not in the controller. Bits 4 and 5 of SCU
 * 0x210 select it, and nothing in a warm reset puts them back, so whatever
 * mode the CP's firmware left the part in survives into a monitor boot.
 */
#define SCU_BASE            0xF1388000ull
#define SCU_OSPI_MODE_STS   (SCU_BASE + 0x210)
#define SCU_OSPI_MODE_SET   (SCU_BASE + 0x214)
#define SCU_OSPI_MODE_CLR   (SCU_BASE + 0x218)
#define SCU_OSPI_MODE_BITS  (BIT(4) | BIT(5))

/*
 * The ROM sets this to 1 in its NOR boot path, immediately before bringing the
 * controller up, and then waits. A monitor boot never runs that path, so
 * without this and the mode bits above the flash reads nothing but 0xFF.
 */
#define SCU_NOR_ENABLE      (SCU_BASE + 0xF20)

#define CFG_ENABLE          BIT(0)
#define CFG_CPOL            BIT(1)
#define CFG_CPHA            BIT(2)
#define CFG_DIRECT_ACCESS   BIT(7)
#define CFG_AHB_DECODER     BIT(8)
#define CFG_ENABLE_DMA      BIT(15)
#define CFG_BAUD_MASK       (0xFu << 19)
#define CFG_BAUD(n)         ((u32)(((n) / 2 - 1) & 0xF) << 19)
#define CFG_IDLE            BIT(31)

#define FLASHCMD_OPCODE(op)     ((u32)(op) << 24)
#define FLASHCMD_RD_ENABLE      BIT(23)
#define FLASHCMD_RD_BYTES(n)    ((u32)((n) - 1) << 20)
#define FLASHCMD_ADDR_ENABLE    BIT(19)
#define FLASHCMD_ADDR_BYTES(n)  ((u32)((n) - 1) << 16)
#define FLASHCMD_DUMMY(n)       ((u32)(n) << 7)
#define FLASHCMD_EXEC_BUSY      BIT(1)
#define FLASHCMD_EXEC           BIT(0)

#define DEVRD_OPCODE(op)        ((u32)(op))
#define DEVRD_ADDR_SINGLE       (0u << 12)
#define DEVRD_DATA_SINGLE       (0u << 16)
#define DEVRD_DUMMY(n)          ((u32)(n) << 24)

#define DEVSZ_ADDR_BYTES(n)     ((u32)((n) - 1) & 0xF)
#define DEVSZ_PAGE(n)           ((u32)(n) << 4)
#define DEVSZ_BLOCK_SHIFT(n)    ((u32)(n) << 16)

#define OP_RDID         0x9F
#define OP_READ4B       0x13
#define OP_READ         0x03

#define BAUD_DIV        4

/* Chip-select setup/hold, as the ROM programs them. */
#define DELAY_VAL       0x06060303u
#define RDDATACAP_VAL   0x21u

/* No timer is set up in a payload, so waits are bounded by iteration count. */
#define POLL_LIMIT      20000u

static int wait_idle(void)
{
    u32 n;

    for (n = 0; n < POLL_LIMIT; n++)
        if (read32(OSPI_CFG) & CFG_IDLE)
            return 0;
    return -1;
}

static void controller_disable(void)
{
    clrbits32(OSPI_CFG, CFG_ENABLE);
    dsb();
}

/*
 * Read back off the controller after the ROM's own bring-up: a known-good
 * register set for booting this flash from an arbitrary prior state.
 */
#define OSPI_DMAPER         (OSPI_BASE + 0x20)
#define OSPI_MODEBIT        (OSPI_BASE + 0x28)
#define OSPI_TXTHRESH       (OSPI_BASE + 0x30)
#define OSPI_RXTHRESH       (OSPI_BASE + 0x34)
#define OSPI_WRCOMPCTRL     (OSPI_BASE + 0x38)
#define OSPI_INDRDWATER     (OSPI_BASE + 0x64)

#define CFG_ROM_VALUE       0x00088041u
/* 0x13 is READ with a 4-byte address; 0x03 reaches only the low 16 MiB
 * and wraps silently above it. */
#define DEVRD_ROM_VALUE     0x00000013u
#define DEVWR_ROM_VALUE     0x00000002u
/* Low nibble is address bytes - 1. */
#define DEVSZ_ROM_VALUE     0x00101003u
#define SRAMPART_ROM_VALUE  0x00000080u
#define DMAPER_ROM_VALUE    0x00000602u
#define MODEBIT_ROM_VALUE   0x00000200u
#define WRCOMP_ROM_VALUE    0x00010005u

static void spin(u32 n)
{
    while (n--)
        __asm__ volatile("" ::: "memory");
}

int ospi_init(void)
{
    write32(SCU_NOR_ENABLE, 1);
    dsb();
    spin(100000);

    write32(SCU_OSPI_MODE_CLR, SCU_OSPI_MODE_BITS);
    dsb();

    if (wait_idle() != 0)
        return -1;

    controller_disable();

    write32(OSPI_DELAY,       DELAY_VAL);
    write32(OSPI_RDDATACAP,   RDDATACAP_VAL);
    write32(OSPI_DEVSZ,       DEVSZ_ROM_VALUE);
    write32(OSPI_DEVRD,       DEVRD_ROM_VALUE);
    write32(OSPI_DEVWR,       DEVWR_ROM_VALUE);
    write32(OSPI_SRAMPART,    SRAMPART_ROM_VALUE);
    write32(OSPI_INDAHBTRIG,  OSPI_AHB_TRIG);
    write32(OSPI_INDRDWATER,  0x40);
    write32(OSPI_REMAPADDR,   0);
    write32(OSPI_DMAPER,      DMAPER_ROM_VALUE);
    write32(OSPI_MODEBIT,     MODEBIT_ROM_VALUE);
    write32(OSPI_TXTHRESH,    1);
    write32(OSPI_RXTHRESH,    1);
    write32(OSPI_WRCOMPCTRL,  WRCOMP_ROM_VALUE);
    write32(OSPI_IRQSTAT,     0xFFFFFFFFu);
    dsb();

    write32(OSPI_CFG, CFG_ROM_VALUE);
    dsb();

    return wait_idle();
}

int ospi_stig(u8 op, u8 dummy, u8 nbytes, u32 addr, int use_addr, u8 *out)
{
    u32 cmd, n, lo, hi, i;

    if (nbytes == 0 || nbytes > 8)
        return -1;
    if (wait_idle() != 0)
        return -1;

    cmd = FLASHCMD_OPCODE(op) | FLASHCMD_RD_ENABLE |
          FLASHCMD_RD_BYTES(nbytes) | FLASHCMD_DUMMY(dummy);
    if (use_addr) {
        write32(OSPI_FLASHCMDADDR, addr);
        cmd |= FLASHCMD_ADDR_ENABLE | FLASHCMD_ADDR_BYTES(3);
    }

    write32(OSPI_FLASHCMD, cmd);
    dsb();
    write32(OSPI_FLASHCMD, cmd | FLASHCMD_EXEC);
    dsb();

    for (n = 0; n < POLL_LIMIT; n++) {
        if (!(read32(OSPI_FLASHCMD) & FLASHCMD_EXEC_BUSY))
            break;
    }
    if (n == POLL_LIMIT)
        return -1;

    lo = read32(OSPI_FLASHRDDATALO);
    hi = read32(OSPI_FLASHRDDATAUP);

    for (i = 0; i < nbytes; i++)
        out[i] = (u8)((i < 4 ? lo >> (8 * i) : hi >> (8 * (i - 4))));
    return 0;
}

#define INDRD_START     BIT(0)
#define INDRD_DONE      BIT(5)

/*
 * Indirect read, which is how the ROM drives this part: the controller fills
 * its internal SRAM and software drains it through the AHB window rather than
 * addressing the flash directly.
 */
int ospi_indirect_read(u32 offset, u64 dst, u32 len)
{
    u32 got = 0, guard = 0;

    if (len == 0 || (len & 3) || offset >= FLASH_SIZE ||
        len > FLASH_SIZE - offset)
        return -1;

    write32(OSPI_INDRD, INDRD_DONE);
    write32(OSPI_INDRDSTADDR, offset);
    write32(OSPI_INDRDCNT, len);
    dsb();
    write32(OSPI_INDRD, INDRD_START);
    dsb();

    /* Drain as it fills, so a transfer may be longer than the controller's
     * SRAM. */
    while (got < len) {
        u32 fill = read32(OSPI_SRAMFILL) & 0xFFFF;

        if (fill == 0) {
            if (++guard > POLL_LIMIT)
                return -1;
            continue;
        }
        guard = 0;

        while (fill-- && got < len) {
            write32(dst + got, read32(OSPI_AHB_BASE));
            got += 4;
        }
    }

    write32(OSPI_INDRD, INDRD_DONE);
    dsb();
    return 0;
}
