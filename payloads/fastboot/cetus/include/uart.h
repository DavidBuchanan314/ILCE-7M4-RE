#ifndef CETUS_UART_H
#define CETUS_UART_H

#include "io.h"

/*
 * PL011 on the CP, wired to the TX/RX pair on the multi connector. The block
 * answers before anything configures it, so its clock is already running.
 * Nothing else drives this line during a normal boot.
 */

#define UART_BASE(ch)   (0xF1000000ull + (u64)(ch) * 0x1000)
#define UART_CHANNELS   4       /* ch4 is not clocked; reading it faults */

void uart_init(u32 ch);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_write(const u8 *p, u32 n);

#endif /* CETUS_UART_H */
