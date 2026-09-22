#ifndef FASTBOOT_DARWIN_H
#define FASTBOOT_DARWIN_H

#include "io.h"

/*
 * Link to Darwin, the system power MCU, over SIO channel 0. Transport and
 * protocol come from the AP loader's own primitive at 0xFE04DD3C.
 *
 * The Virtual WDT is why this exists: Darwin counts four one-byte
 * down-counters at 0x200026FC and kills the system when one reaches zero.
 * Nothing in bootrom context re-arms them.
 */

#define DARWIN_PKT      0x80    /* packet size, both directions */

/* Darwin RAM. Command 0x16 bounds writes to [0x20000000, 0x20004000]. */
#define DARWIN_RAM_BASE 0x20000000u

/* The Virtual WDT counter table: 4 bytes, 0xFF = disabled. */
#define VWDT_TABLE      0x200026FCu

/* Per-channel reload bytes, loaded by every vwdt_set call site. Zero makes
 * vwdt_set a no-op. */
#define VWDT_RELOAD     0x2000003Cu

/* Idle the chip select and hand the clock and data pads to the SIO block.
 * Sends nothing. */
void darwin_init(void);

/*
 * One raw 0x80-byte transaction. Seals tx's CRC in place (tx must be
 * writable), then repeats the exchange until a reply validates and its first
 * `match_len` bytes echo tx, or `attempts` exchanges have gone by.
 *
 * `match_len` identifies the reply: 1 checks only the opcode, PKT_HDR (6) also
 * pins the address and length.
 *
 * Bounded by exchanges rather than time because Darwin streams status frames
 * of its own (command 0x10) whether or not it has an answer.
 *
 * Retrying re-sends the same packet, so every command used here is idempotent.
 */
int darwin_xfer(u8 *tx, u8 *rx, u32 match_len, u32 attempts);

#define DARWIN_ATTEMPTS 8   /* ~11 ms; short enough never to starve USB */

#define PKT_HDR 6       /* opcode + 4-byte address + length */

/* Command 0x16: read `len` (<= 0x78) bytes from any Darwin address. Reads
 * are NOT bounds-checked by the firmware, so this reaches flash too. */
int darwin_read(u32 addr, u8 *buf, u32 len);

/*
 * Command 0x12: write `len` (<= 0x78) bytes at `off` from the config base.
 * The firmware requires `off` to be a multiple of 0x78.
 */
int darwin_write_block(u32 off, const u8 *buf, u32 len);

/* Read the four live counters and the three reload bytes. Either pointer may
 * be null. */
int darwin_vwdt_state(u8 counters[4], u8 reloads[3]);

#endif /* FASTBOOT_DARWIN_H */
