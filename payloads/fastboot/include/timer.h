#ifndef FASTBOOT_TIMER_H
#define FASTBOOT_TIMER_H

#include "io.h"

/*
 * SoC timer0. Both the register map and the start sequence are lifted straight
 * out of the mask ROM, which is the strongest possible provenance -- this is
 * the timer the ROM itself uses to time the fatal-handler LED blinks.
 *
 *   0xFFFF14AC  (start)          0xFFFF5E08  (wait until count >= target)
 *       w8 = 0xF1042000              w8 = 0xF104200C
 *       str 0x30,   [x8]             ldr w9, [x8]
 *       str 0x10,   [x8, #4]         ...spin while count < target...
 *       str 0x1030, [x8]
 *
 * There is an identical second timer at 0xF1043000 (ROM routines 0xFFFF14D4 /
 * 0xFFFF5E3C); the ROM's serial-boot receiver uses that one, so timer0 is the
 * safer of the two to take over.
 *
 * Rate: 4.000 MHz, 1 tick = 250 ns. Measured in RP2040_SERIALBOOT.md by
 * bisecting the ROM's own 909-tick preamble threshold with a swept square wave.
 * Corroborated by the fatal handler's blink constants: 0x1E8480 = 2,000,000
 * ticks = 0.5 s long flash, 0xF4240 = 0.25 s short, 0x7A1200 = 2 s gap, which
 * is exactly the cadence BOOT.md describes seeing on hardware.
 *
 * We program the registers directly rather than calling the ROM routines: the
 * values are known, and this avoids depending on ROM calling conventions or on
 * the ROM's timer0-restart semantics (its delay is relative to the last start,
 * which is why the blink loop restarts the timer before every wait).
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

/* Raw 32-bit counter. Wraps every ~1073 s at 4 MHz; udelay() handles that. */
static inline u32 timer_ticks(void)
{
    return read32(TIMER0_COUNT);
}

#endif /* FASTBOOT_TIMER_H */
