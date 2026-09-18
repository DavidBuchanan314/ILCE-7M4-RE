#ifndef CETUS_MONITOR_H
#define CETUS_MONITOR_H

#include "io.h"

/*
 * Opcodes. The first three keep the ROM's meaning so an unmodified client can
 * talk to either; the rest are ours, and are what the payload exists for.
 */
#define OP_STATUS       0x11    /* liveness                                  */
#define OP_UPLOAD       0x22    /* +1 width log2, +8 addr, +12 count         */
#define OP_DOWNLOAD     0x21    /* +1 width log2, +8 addr, +12 count         */
#define OP_NOR_READ     0x30    /* +8 flash offset, +12 byte count           */
#define OP_NOR_CMD      0x32    /* +1 op, +2 dummy, +3 len, +8 addr, +12 flags */
#define OP_UART_TX      0x33    /* +8 addr, +12 len -- send that text on the UART */

#define RES_OK          0x81
#define RES_ERR         0x82

__attribute__((noreturn)) void monitor_loop(void);

#endif /* CETUS_MONITOR_H */
