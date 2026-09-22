#include "timer.h"
#include "io.h"

void timer_init(void)
{
    /* Exactly the ROM's sequence at 0xFFFF14AC. */
    write32(TIMER0_CTRL,   TIMER0_CTRL_STOP);
    write32(TIMER0_RELOAD, 0x10);
    write32(TIMER0_CTRL,   TIMER0_CTRL_RUN);
}

/* Relative to now, so it nests; the unsigned subtraction survives the wrap. */
void udelay(u32 usec)
{
    u32 start = timer_ticks();
    u32 ticks = usec * TIMER0_TICKS_PER_US;

    while ((timer_ticks() - start) < ticks)
        __asm__ volatile("nop");
}

void mdelay(u32 msec)
{
    /* msec * 4000 would overflow a u32 at ~1073 s. */
    while (msec--)
        udelay(1000);
}
