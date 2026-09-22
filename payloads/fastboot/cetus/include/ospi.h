#ifndef CETUS_OSPI_H
#define CETUS_OSPI_H

#include "io.h"

/*
 * Cadence OSPI controller driving a 64 MiB octal NOR that powers up in
 * 1S-1S-1S.
 *
 * The part on this unit reports JEDEC id 2C 5B 1A (Micron, Xccela octal,
 * 512 Mbit); expect the second source, Macronix MX25UM51245G id C2 80 3A, in
 * other units. Both are 64 MiB and both answer the read this driver uses.
 *
 * Nothing here issues a write, erase or write-enable opcode. The flash is the
 * CP's only boot medium and cannot currently be recovered if it is damaged.
 */

#define OSPI_BASE       0xF1054000ull

/*
 * The AHB data window the CPU reads. The controller's trigger register holds
 * only the low 28 bits of it, so the address programmed into the hardware and
 * the address loaded from are not the same.
 */
#define OSPI_AHB_BASE   0xDFFE0000ull
#define OSPI_AHB_TRIG   (u32)(OSPI_AHB_BASE & 0x0FFFFFFFu)
#define OSPI_AHB_SIZE   0x10000u

#define FLASH_SIZE      0x4000000u      /* 64 MiB */

int  ospi_init(void);
/*
 * One register-driven flash command, up to 8 bytes of reply: RDID, SFDP,
 * status. Issues exactly the opcode it is given, with a 3-byte address.
 */
int  ospi_stig(u8 op, u8 dummy, u8 nbytes, u32 addr, int use_addr, u8 *out);
int  ospi_indirect_read(u32 offset, u64 dst, u32 len);

#endif /* CETUS_OSPI_H */
