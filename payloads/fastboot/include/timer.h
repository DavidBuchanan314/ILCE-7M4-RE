#ifndef FASTBOOT_TIMER_H
#define FASTBOOT_TIMER_H

#include "io.h"

/*
 * SoC timer0, the one the mask ROM uses for its fatal-handler LED blinks.
 * Register map and start sequence are the ROM's, at 0xFFFF14AC.
 *
 * There is an identical second timer at 0xF1043000; the ROM's serial-boot
 * receiver uses that one, so timer0 is the safer of the two to take over.
 *
 * Rate: 4.000 MHz, 1 tick = 250 ns.
 */
#define TIMER0_BASE     0xF1042000ull
#define TIMER0_CTRL     (TIMER0_BASE + 0x00)
#define TIMER0_RELOAD   (TIMER0_BASE + 0x04)  /* ROM writes 0x10; purpose unidentified */
#define TIMER0_COUNT    (TIMER0_BASE + 0x0C)  /* free-running up-counter */

#define TIMER0_CTRL_STOP    0x0030
#define TIMER0_CTRL_RUN     0x1030

#define TIMER0_HZ           4000000u
#define TIMER0_TICKS_PER_US 4u

void timer_init(void);

/* Wraps every ~1073 s at 4 MHz. */
static inline u32 timer_ticks(void)
{
    return read32(TIMER0_COUNT);
}

#endif /* FASTBOOT_TIMER_H */
