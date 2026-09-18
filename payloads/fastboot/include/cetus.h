#ifndef FASTBOOT_CETUS_H
#define FASTBOOT_CETUS_H

#include "io.h"

#define CETUS_FRAME     16
#define CETUS_OK        0x81

#define CETUS_E_RDY     (-1)
#define CETUS_E_WRITE   (-2)
#define CETUS_E_ACK     (-3)
#define CETUS_E_RES     (-4)
#define CETUS_E_PAY     (-5)
#define CETUS_E_ARG     (-6)
#define CETUS_E_SHORT   (-7)

/* A status byte the target reported but we did not expect. */
#define CETUS_IS_STATUS(rc)  ((rc) <= -0x100)
#define CETUS_STATUS(rc)     ((u8)(-(rc) & 0xFF))

/* Where the bundled CP payload is staged and entered. */
#define CETUS_PAYLOAD_BASE  0xFE020000u

void cetus_spi_init(void);

/* sel picks the 48/96/192 MHz source; SCK = source / (cpsdvsr * (1 + scr)). */
int cetus_set_rate(u32 sel, u32 cpsdvsr, u32 scr);
u32 cetus_rate_khz(void);

/* Only safe once our payload is answering; see cetus.c. */
void cetus_set_pipelined(int on);

/* Resets the CP into SPI-COM boot mode, where its mask ROM runs the monitor. */
void cetus_monitor_boot(void);

u32 cetus_reset_state(void);
u32 cetus_bootmode_state(void);

int cetus_status(void);

/* width is 1, 2 or 4; addr must be aligned to it and len a multiple of it. */
int cetus_read(u32 addr, u8 *buf, u32 len, u32 width);
int cetus_write(u32 addr, const u8 *buf, u32 len, u32 width);

/* Answers before handing over. The ROM's monitor is gone once it does; the
 * payload in payloads/cetus puts an equivalent one back on the same link. */
int cetus_entry(u32 addr);

/*
 * Served by that payload, not by the ROM. The flash is only reachable from
 * code running on the CP, so these are how the AP sees it at all.
 */
int cetus_nor_read(u32 offset, u8 *buf, u32 len);
int cetus_nor_id(u32 *id);

/* One flash command, up to 8 bytes back. Issues exactly the opcode given. */
int cetus_nor_cmd(u8 op, u8 dummy, u8 nbytes, u32 addr, int use_addr, u8 *out);

/*
 * Push the bundled payload to the CP and enter it. Done during init, so the
 * flash commands work without anything being staged by hand.
 */
int cetus_payload_start(void);

/* Result of the last cetus_bring_up(), for reporting. 0 = payload running. */
int cetus_payload_state(void);

/* Reset the CP into the ROM monitor, then put our payload back on it. */
int cetus_bring_up(void);

#endif /* FASTBOOT_CETUS_H */
