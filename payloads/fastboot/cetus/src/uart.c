#include "io.h"
#include "uart.h"

/*
 * Register layout, divisors, pad masks and write order all come from the CP
 * loader's own uart_init, read out of eSRAM: the mask ROM decrypts that image
 * in as it boots and a reset into the monitor leaves it there.
 */

static u64 uart_base = UART_BASE(0);

#define UART_DR         (uart_base + 0x00)
#define UART_ECR        (uart_base + 0x04)
#define UART_FR         (uart_base + 0x18)
#define UART_IBRD       (uart_base + 0x24)
#define UART_FBRD       (uart_base + 0x28)
#define UART_LCRH       (uart_base + 0x2C)
#define UART_CR         (uart_base + 0x30)
#define UART_IMSC       (uart_base + 0x38)
#define UART_ICR        (uart_base + 0x44)

#define FR_BUSY         BIT(3)
#define FR_TXFF         BIT(5)

#define LCRH_VALUE      0x70u       /* 8 bits, no parity, FIFOs on */
#define CR_VALUE        0xF01u      /* UARTEN | TXE | RXE | DTR | RTS */

/*
 * UARTCLK is 12 MHz, which is the whole reason a first attempt at 48 read as
 * silence: BAUDDIV = 6 + 33/64 gives 12e6 / (16 * 6.5156) = 115200. The
 * loader's other mode is 1 + 40/64, i.e. 460800.
 */
#define BRD_INT_115200  6u
#define BRD_FRAC_115200 0x21u

/*
 * The pads sit on GPIO port 3, two adjacent bits per channel: both go to the
 * hardware function, and only the receive one needs its input buffer enabled.
 * Without this the UART shifts bytes out perfectly and none of them reach a
 * pin -- the same trap SPI0 has.
 */
#define UART_GPIO       (0xF101D000ull + 3ull * 0x1000)
#define GPIO_INEN_SET   (UART_GPIO + 0x24)
#define GPIO_FUNC_CLR   (UART_GPIO + 0x38)

static const u32 pad_mask[UART_CHANNELS] = { 0x0003u, 0x000Cu, 0x00C0u, 0x0300u };
static const u32 rx_mask[UART_CHANNELS]  = { 0x0001u, 0x0004u, 0x0040u, 0x0100u };

#define TX_POLL_LIMIT   100000u

void uart_init(u32 ch)
{
    if (ch >= UART_CHANNELS)
        return;
    uart_base = UART_BASE(ch);

    write32(GPIO_INEN_SET, rx_mask[ch]);
    write32(GPIO_FUNC_CLR, pad_mask[ch]);
    dsb();

    /* Masked rather than the loader's 0x40: there is no vector table here, so
     * an interrupt would be fatal and nothing services one anyway. */
    write32(UART_IMSC, 0);
    write32(UART_IBRD, BRD_INT_115200);
    write32(UART_FBRD, BRD_FRAC_115200);
    write32(UART_LCRH, LCRH_VALUE);
    write32(UART_ECR, 0);
    write32(UART_ICR, 0xFFF);
    dsb();

    write32(UART_CR, CR_VALUE);
    dsb();
}

void uart_putc(char c)
{
    u32 n;

    for (n = 0; n < TX_POLL_LIMIT; n++) {
        if (!(read32(UART_FR) & FR_TXFF))
            break;
    }
    write32(UART_DR, (u8)c);
}

void uart_write(const u8 *p, u32 n)
{
    while (n--)
        uart_putc((char)*p++);
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}
