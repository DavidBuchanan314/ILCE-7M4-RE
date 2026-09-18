#ifndef CETUS_UART_H
#define CETUS_UART_H

#include "io.h"

/*
 * PL011 on the CP, wired to the TX/RX pair on the multi connector.
 *
 * Identified from the block itself rather than from any firmware: the
 * PrimeCell id at the top of the window reads 0D F0 05 B1 with a peripheral
 * id of ARM part 0x011. It answers before anything configures it, so its
 * clock is already running -- nothing here has to be ungated.
 *
 * Nothing drives this line during a normal boot (checked: silent at every
 * common rate from 38400 to 921600), so there is nobody to talk over.
 */

#define UART_BASE(ch)   (0xF1000000ull + (u64)(ch) * 0x1000)
#define UART_CHANNELS   4       /* ch4 is not clocked; reading it faults */

void uart_init(u32 ch);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_write(const u8 *p, u32 n);

#endif /* CETUS_UART_H */
