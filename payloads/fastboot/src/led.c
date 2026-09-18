#include "led.h"
#include "io.h"
#include "timer.h"

/*
 * Timings are chosen to be countable by a person watching, not compact.
 * A blip is a full second: 0.3 s lit, 0.7 s dark. The marker is a long 1.5 s
 * flash followed by 1.5 s of dark, so the start of a count is unmistakable,
 * and cycles are separated by 3 s of dark.
 *
 * An earlier version used 50 ms units, which turned out to be far too fast to
 * count by eye -- the whole point of this channel is that a human can read it.
 */
#define BLIP_ON_MS      300
#define BLIP_OFF_MS     700
#define MARKER_ON_MS    1500
#define MARKER_OFF_MS   1500
#define CYCLE_GAP_MS    3000

typedef void (*rom_fn_t)(void);

void led_init(void)
{
    /* The mask ROM already knows how to mux and enable these pins; borrowing
     * its routine is cheaper and safer than rediscovering the pinmux. This is
     * the same call the LED PoC makes. */
    ((rom_fn_t)ROM_LED_INIT)();
}

void led_on(void)
{
    write32(LED_GPIO_ON, LED_BIT);
}

void led_off(void)
{
    write32(LED_GPIO_OFF, LED_BIT);
}

static void pulse(u32 on_ms, u32 off_ms)
{
    led_on();
    mdelay(on_ms);
    led_off();
    mdelay(off_ms);
}

void led_selftest(void)
{
    int i;

    /* Long, slow, and symmetric: 1 s on, 1 s off, three times. Anything other
     * than three distinct flashes means led_off() is not working. */
    for (i = 0; i < 3; i++) {
        led_on();
        mdelay(1000);
        led_off();
        mdelay(1000);
    }
}

void led_blip(void)
{
    pulse(120, 0);
}

void led_marker(void)
{
    pulse(MARKER_ON_MS, MARKER_OFF_MS);
}

void led_count(u32 n)
{
    led_marker();
    while (n--)
        pulse(BLIP_ON_MS, BLIP_OFF_MS);
    mdelay(CYCLE_GAP_MS);
}

void led_fail(u32 code)
{
    for (;;)
        led_count(code);
}

/*
 * Long enough to see a single packet, short enough that a busy link reads as
 * lit rather than as flicker.
 */
#define ACTIVITY_MS     30

static u32 act_start;
static int act_lit;

void led_activity(void)
{
    act_start = timer_ticks();
    if (!act_lit) {
        led_on();
        act_lit = 1;
    }
}

void led_activity_tick(void)
{
    if (!act_lit)
        return;
    if (timer_ticks() - act_start < ACTIVITY_MS * (TIMER0_HZ / 1000))
        return;
    led_off();
    act_lit = 0;
}

void led_panic(void)
{
    for (;;)
        pulse(100, 100);
}
