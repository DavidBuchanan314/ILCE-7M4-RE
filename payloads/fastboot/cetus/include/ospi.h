#ifndef CETUS_OSPI_H
#define CETUS_OSPI_H

#include "io.h"

/*
 * Cadence OSPI controller driving a 64 MiB octal NOR that powers up in
 * 1S-1S-1S with 3-byte addressing.
 *
 * The part on this unit reports JEDEC id 2C 5B 1A -- Micron, Xccela octal,
 * 512 Mbit -- which SFDP corroborates: a valid table at offset 0 whose
 * density field reads 0x1FFFFFFF, one bit short of 2^29 bits. Expect the
 * second source (Macronix MX25UM51245G, id C2 80 3A) in other units; both are
 * 64 MiB and both answer the plain 0x03 read this driver uses.
 *
 * 3-byte addressing only spans the first 16 MiB. Reaching the rest needs
 * 4-byte opcodes, which is a change to DEVRD and DEVSZ, not to the framing.
 *
 * Nothing here issues a write, erase or write-enable opcode. The flash is the
 * CP's only boot medium and cannot currently be recovered if it is damaged.
 */

#define OSPI_BASE       0xF1054000ull

/* The controller's AHB data window, as the ROM configures it. */
/*
 * The window the CPU reads. The controller's trigger register only holds the
 * low 28 bits of it -- the ROM masks the value on the way in -- so the address
 * programmed into the hardware and the address loaded from are not the same.
 */
#define OSPI_AHB_BASE   0xDFFE0000ull
#define OSPI_AHB_TRIG   (u32)(OSPI_AHB_BASE & 0x0FFFFFFFu)
#define OSPI_AHB_SIZE   0x10000u

#define FLASH_SIZE      0x4000000u      /* 64 MiB */

int  ospi_init(void);
/*
 * One register-driven flash command, up to 8 bytes of reply. This is how the
 * part is identified and interrogated -- RDID, SFDP, status -- without
 * touching the read path, and it issues exactly the opcode it is given.
 */
int  ospi_stig(u8 op, u8 dummy, u8 nbytes, u32 addr, int use_addr, u8 *out);
int  ospi_indirect_read(u32 offset, u64 dst, u32 len);

#endif /* CETUS_OSPI_H */
