#ifndef FASTBOOT_LED_H
#define FASTBOOT_LED_H

#include "io.h"

/*
 * GPIO ch18. Five 16-byte banks of {status, set, clear, --}, set and clear
 * write-only. The pin is active low.
 */
#define LED_GPIO_STS    0xF102F040ull
#define LED_GPIO_OFF    0xF102F044ull   /* bank 4 SET   -> pin high -> dark */
#define LED_GPIO_ON     0xF102F048ull   /* bank 4 CLEAR -> pin low  -> lit  */
#define LED_BIT         BIT(0)

/* Mask ROM routine that muxes and enables the LED pins. */
#define ROM_LED_INIT    0xFFFF0784ull

void led_init(void);
void led_on(void);
void led_off(void);

/* One short flash. The only sign of life before the UART log exists. */
void led_blip(void);

/* Marker pulse, framing a count so its start is unambiguous. */
void led_marker(void);

/* N short flashes, once. */
void led_count(u32 n);

/* N short flashes, forever. */
__attribute__((noreturn)) void led_fail(u32 code);

/*
 * USB activity indicator, dark when idle. led_activity_tick() blinks while
 * traffic keeps coming and puts the LED out once it stops. Neither waits.
 */
void led_activity(void);
void led_activity_tick(void);

/* Fast even blinking, forever. */
__attribute__((noreturn)) void led_panic(void);

#endif /* FASTBOOT_LED_H */
