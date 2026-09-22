#ifndef FASTBOOT_FASTBOOT_H
#define FASTBOOT_FASTBOOT_H

#include "io.h"

/* Assumes the device is configured and the bulk endpoints are live. */
__attribute__((noreturn)) void fastboot_loop(void);

#endif /* FASTBOOT_FASTBOOT_H */
