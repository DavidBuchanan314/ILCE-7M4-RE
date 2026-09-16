#ifndef FASTBOOT_RESET_H
#define FASTBOOT_RESET_H

#include "io.h"

/* Assert XRESET_REQ. Immediate; nothing after the call runs. */
__attribute__((noreturn)) void system_reset(void);

#endif /* FASTBOOT_RESET_H */
