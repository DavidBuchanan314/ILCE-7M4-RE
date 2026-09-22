#ifndef CETUS_MBOX_H
#define CETUS_MBOX_H

#include "io.h"

/*
 * Bring-up record at a fixed address, so it can be found with no symbols.
 * `step` advances past each point of no return, which is the only way a
 * payload that wedges before the link is serving can say where it got to.
 */

#define MBOX_ADDR       0xFE030000u
#define MBOX_MAGIC      0x53555443u     /* "CTUS" little-endian */

enum mbox_step {
    STEP_ENTERED     = 1,
    STEP_READY       = 4,
    STEP_CMD_START   = 5,
    STEP_HALTED      = 9,

    STEP_ARM1        = 0x40,
    STEP_ARM2        = 0x41,
    STEP_DRAIN       = 0x42,
    STEP_RECV        = 0x43,
    STEP_RECV_TMO    = 0x44,
    STEP_RESYNCED    = 0x45,
    STEP_ACK         = 0x46,
    STEP_DISPATCH    = 0x47,
};

enum mbox_status {
    ST_RUNNING   = 0,
    ST_OK        = 1,
    ST_BAD_CMD   = 2,
    ST_TIMEOUT   = 3,
    ST_BAD_ARG   = 4,
};

struct mbox {
    u32 magic;
    u32 version;
    u32 step;
    u32 status;
    u32 cmd;
    u32 arg[4];
    u32 out[4];
};

#define MBOX_VERSION 1

extern volatile struct mbox mbox;

static inline void mbox_step(u32 s)
{
    mbox.step = s;
    dsb();
}

#endif /* CETUS_MBOX_H */
