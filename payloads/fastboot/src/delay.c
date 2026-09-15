#include "timer.h"
#include "io.h"

void timer_init(void)
{
    /* Exactly the ROM's sequence at 0xFFFF14AC. */
    write32(TIMER0_CTRL,   TIMER0_CTRL_STOP);
    write32(TIMER0_RELOAD, 0x10);
    write32(TIMER0_CTRL,   TIMER0_CTRL_RUN);
}

/*
 * Unlike the ROM's delay, this is relative to *now* rather than to the last
 * timer restart, so it nests and interleaves safely and never needs to disturb
 * the running counter.
 *
 * The unsigned subtraction is deliberate: it stays correct across the 32-bit
 * wrap, so a long LED output run cannot hang in here.
 */
void udelay(u32 usec)
{
    u32 start = timer_ticks();
    u32 ticks = usec * TIMER0_TICKS_PER_US;

    while ((timer_ticks() - start) < ticks)
        __asm__ volatile("nop");
}

void mdelay(u32 msec)
{
    /* Split rather than multiplying up: msec * 4000 would overflow a u32 at
     * about 1073 s, and this keeps each udelay well inside a counter wrap. */
    while (msec--)
        udelay(1000);
}
