#include "led.h"
#include "io.h"
#include "timer.h"

/* Slow enough to count by eye. */
#define BLIP_ON_MS      300
#define BLIP_OFF_MS     700
#define MARKER_ON_MS    1500
#define MARKER_OFF_MS   1500
#define CYCLE_GAP_MS    3000

typedef void (*rom_fn_t)(void);

void led_init(void)
{
    /* Cheaper than rediscovering the pinmux. Leaves the LED off. */
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

#define MS_TICKS(ms)    ((ms) * (TIMER0_HZ / 1000))

/* Must outlast the gaps inside a transfer: a NOR dump spends ~100 ms per chunk
 * on the CP link with nothing pumping USB events. */
#define ACTIVITY_HOLD_MS 200

/* 2^18 ticks is 65.5 ms at 4 MHz. */
#define ACTIVITY_BLINK_SHIFT 18

static u32 act_last;
static int act_busy;
static int act_lit;

static void act_set(int on)
{
    act_lit = on;
    if (on)
        led_on();
    else
        led_off();
}

void led_activity(void)
{
    act_last = timer_ticks();
    act_busy = 1;
}

void led_activity_tick(void)
{
    u32 now;
    int want;

    if (!act_busy)
        return;

    now = timer_ticks();

    if (now - act_last >= MS_TICKS(ACTIVITY_HOLD_MS)) {
        act_busy = 0;
        act_set(0);
        return;
    }

    want = (int)((now >> ACTIVITY_BLINK_SHIFT) & 1);
    if (want != act_lit)
        act_set(want);
}

void led_panic(void)
{
    for (;;)
        pulse(100, 100);
}
