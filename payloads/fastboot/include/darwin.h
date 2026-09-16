#ifndef FASTBOOT_DARWIN_H
#define FASTBOOT_DARWIN_H

#include "io.h"

/*
 * Link to Darwin, the system power MCU, over SIO channel 0.
 *
 * Transport, register sequence and protocol are lifted from the AP's own
 * transaction primitive at 0xFE04DD3C in loader.bin -- see darwin.c.
 *
 * The point of all this is the Virtual WDT: Darwin counts four one-byte
 * down-counters at 0x200026FC and kills the system when one reaches zero.
 * In bootrom context nothing re-arms them, so a payload eventually dies.
 */

#define DARWIN_PKT      0x80    /* packet size, both directions */

/* Darwin RAM. Command 0x16 bounds writes to [0x20000000, 0x20004000]. */
#define DARWIN_RAM_BASE 0x20000000u

/* The Virtual WDT counter table: 4 bytes, 0xFF = disabled. */
#define VWDT_TABLE      0x200026FCu

/* Per-channel reload bytes in the config struct, loaded by every vwdt_set
 * call site. Zero means vwdt_set is a no-op, so these also say whether the
 * watchdog was ever armed at all. */
#define VWDT_RELOAD     0x2000003Cu

/*
 * Bring up the chip select and note the SIO block's idle state. Cheap and
 * side-effect-free on Darwin -- it sends nothing.
 */
void darwin_init(void);

/*
 * One raw 0x80-byte transaction. Seals tx's CRC in place (so tx must be
 * writable), then retries until a reply arrives whose CRC validates and whose
 * first `match_len` bytes echo tx, or `timeout_ms` elapses.
 *
 * `match_len` is how the reply is identified. 1 checks only the opcode, which
 * is what the AP does; PKT_HDR (6) also pins the address and length, which is
 * what distinguishes two reads of different addresses.
 *
 * `attempts` is a COUNT, not a time budget. Darwin streams status frames of
 * its own (command 0x10) whether or not it has an answer for us, so a loop
 * that waits for an echo can spin indefinitely -- bounding it by exchanges
 * keeps a command that will never be answered from taking the device with it.
 *
 * Retrying re-sends the same packet, so Darwin may execute the command more
 * than once -- every command used here is idempotent.
 *
 * Returns 0, or negative on timeout.
 */
int darwin_xfer(u8 *tx, u8 *rx, u32 match_len, u32 attempts);

#define DARWIN_ATTEMPTS 8   /* ~11 ms; short enough never to starve USB */

#define PKT_HDR 6       /* opcode + 4-byte address + length */

/* Command 0x16: read `len` (<= 0x78) bytes from any Darwin address. Reads
 * are NOT bounds-checked by the firmware, so this reaches flash too. */
int darwin_read(u32 addr, u8 *buf, u32 len);

/*
 * Command 0x12: write `len` (<= 0x78) bytes at `off` from the config base.
 * `off` must be a multiple of 0x78 -- that is the firmware's constraint, not
 * ours. Unlike command 0x16's write path this copies exactly `len` bytes.
 */
int darwin_write_block(u32 off, const u8 *buf, u32 len);

/* Read the four live counters and the three reload bytes. Either pointer may
 * be null. This is the diagnostic that says whether the watchdog is armed. */
int darwin_vwdt_state(u8 counters[4], u8 reloads[3]);

#endif /* FASTBOOT_DARWIN_H */
