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
 * Heartbeat cycle: a long marker flash, a pause, then `code` short blips, then
 * a long dark gap. The marker exists so the count has an unambiguous starting
 * point -- without it there is no way to tell which flash is the first.
 */
#define HB_MARKER_ON_MS 1200
#define HB_MARKER_OFF_MS 600
#define HB_BLIP_ON_MS   250
#define HB_BLIP_OFF_MS  350
#define HB_GAP_MS       2500

static u32 hb_code;
static u32 hb_start;
static u32 hb_step;
static int hb_init;

/*
 * step 0        marker on
 * step 1        marker off
 * steps 2..2N+1 blips (even = on, odd = off)
 * step 2N+2     long gap, dark
 */
void led_heartbeat(u32 code)
{
    u32 now = timer_ticks();
    u32 last = 2 * code + 2;
    u32 want_ms;

    if (!hb_init || code != hb_code) {
        hb_init = 1;
        hb_code = code;
        hb_step = 0;
        hb_start = now;
        led_on();
        return;
    }

    if (hb_step == 0)
        want_ms = HB_MARKER_ON_MS;
    else if (hb_step == 1)
        want_ms = HB_MARKER_OFF_MS;
    else if (hb_step >= last)
        want_ms = HB_GAP_MS;
    else
        want_ms = (hb_step & 1) ? HB_BLIP_OFF_MS : HB_BLIP_ON_MS;

    /* Unsigned subtraction stays correct across the counter wrap. */
    if ((now - hb_start) < want_ms * (TIMER0_HZ / 1000))
        return;

    hb_start = now;
    hb_step++;
    if (hb_step > last)
        hb_step = 0;

    if (hb_step == 0)
        led_on();                                   /* next marker */
    else if (hb_step < last && hb_step >= 2 && !(hb_step & 1))
        led_on();                                   /* blip on */
    else
        led_off();
}

static u32 spin_count;
static int spin_on;

void led_spin_tick(void)
{
    /* ~a few Hz at typical spin rates; no timer involved. */
    if ((++spin_count & 0x3ffff) != 0)
        return;
    spin_on = !spin_on;
    if (spin_on)
        led_on();
    else
        led_off();
}

void led_panic(void)
{
    for (;;)
        pulse(100, 100);
}
