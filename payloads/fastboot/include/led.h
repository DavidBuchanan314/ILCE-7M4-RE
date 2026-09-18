#ifndef FASTBOOT_LED_H
#define FASTBOOT_LED_H

#include "io.h"

/*
 * The LED is the only debug channel available from bootrom payload context, so
 * this is our printf. Everything here is deliberately slow and unambiguous.
 *
 * GPIO ch18 (base 0xf101d000 + 0x1000*18 = 0xF102F000). Dumped from a live
 * camera, the block reads non-zero at +0x00/0x10/0x20/0x30/0x40 and zero at
 * every +4/+8/+C -- five 16-byte banks of {status, set, clear, --}, with set
 * and clear write-only. Same house style as the SCU.
 *
 * The register assignment is taken from the mask ROM, not inferred. The fatal
 * handler's blink loop at 0xFFFF4594, with x21 = 0xF102F044 and w22 = 1:
 *
 *      str w22, [x21, #4]      ; 0xF102F048   flash begins  -> LED ON
 *      <long delay>
 *      str w22, [x21]          ; 0xF102F044   flash ends    -> LED OFF
 *      <long delay>
 *      cbnz w23, loop
 *
 * ON is 0xF102F048 and OFF is 0xF102F044. That also matches the working PoC
 * (tools/uart_boot_poc/payload/payload.S writes 1 to 0xF102F044+4 to light it)
 * and the live register dump, where bit 0 of the status register reads 1 while
 * the LED is dark -- so the pin is active-low and 0xF102F048 is the clear
 * register. Three independent sources agree.
 *
 * ROM_LED_INIT additionally leaves the LED off: it writes 1 to 0xF102F044 as
 * part of its setup.
 */
#define LED_GPIO_STS    0xF102F040ull
#define LED_GPIO_OFF    0xF102F044ull   /* bank 4 SET   -> pin high -> dark */
#define LED_GPIO_ON     0xF102F048ull   /* bank 4 CLEAR -> pin low  -> lit  */
#define LED_BIT         BIT(0)

/*
 * Mask ROM routine that muxes and enables the LED pins (0xFFFF0784). It pokes
 * 0xF102F044/+0x004/0x028/0x034 and SCU 0xF1388404+0/224/452/676/900.
 */
#define ROM_LED_INIT    0xFFFF0784ull

void led_init(void);
void led_on(void);
void led_off(void);

/*
 * Three slow, evenly spaced 1 s flashes. Cheap first-run confirmation that the
 * payload is executing at all and that the timer is roughly the rate we think
 * it is -- if these come out obviously faster or slower than one second, the
 * 4 MHz assumption in timer.h is wrong and every delay below is scaled with it.
 *
 * The LED is for coarse state only: alive, which checkpoint, which failure.
 * Register values are not blinked out -- anything worth reading is either
 * already known from the dumps and kernel source, or observable from the host
 * once the D+ pullup comes up.
 */
void led_selftest(void);

/* One short flash. Cheap proof the payload is executing, for the window
 * before the UART log exists. */
void led_blip(void);

/* Marker pulse, used to frame a count so the start is unambiguous. */
void led_marker(void);

/* N short flashes, once. Coarse "reached checkpoint N" milestones. */
void led_count(u32 n);

/* N short flashes, repeating forever. Terminal state for a known failure,
 * where N identifies which one. */
__attribute__((noreturn)) void led_fail(u32 code);

/*
 * Non-blocking status blinker. Call it often -- from inside the event loop --
 * and it advances its own pattern from the timer, never waiting.
 *
 * This exists because every blocking LED routine here doubles as a way to kill
 * the thing being debugged: led_fail() loops forever and stops servicing USB,
 * which takes ep0 (and with it the status-descriptor channel) down too. A
 * heartbeat that never blocks reports state no matter what else is wedged.
 *
 * Pattern: `code` short blips, then a 2 s gap, repeating.
 */
void led_heartbeat(u32 code);

/*
 * Timer-INDEPENDENT liveness toggle. Driven purely by a call counter, so it
 * blinks whenever the CPU is executing, even if timer0 has stopped. That is
 * the one thing led_heartbeat() cannot report: every path in it except the
 * first call needs timer_ticks() to advance, so a frozen timer and a stalled
 * CPU both present as a solid LED.
 *
 *   blinking  -> CPU is looping; suspect the timer
 *   solid     -> CPU is not executing; suspect a stalled MMIO access
 */
void led_spin_tick(void);

/* Fast even blinking, forever. Terminal state for an unrecoverable error. */
__attribute__((noreturn)) void led_panic(void);

#endif /* FASTBOOT_LED_H */
