#include "io.h"
#include "reset.h"

/*
 * XRESET_REQ -- reset request to Darwin, the system power MCU. A real pin;
 * Darwin latches the edge and resets the SoC, so this needs no clocks and no
 * prior state. DARWIN.md 7a.
 *
 * Same three writes as cxd90057_reset() in
 * drivers/udif/mach-cxd900xx/pm_core.c, with GPIO(x) = 0xF101D000 + 0x1000*x
 * and the bank layout from mach/regs-gpio.h (DIR +0x00, FUNC +0x30,
 * WDATA +0x40, each with SET at +4 and CLR at +8).
 *
 * The order is load-bearing: the pad mux goes last, so the line is never
 * driven through whatever WDATA happened to hold.
 */
#define GPIO0_BASE      0xF101D000ull
#define GPIO0_DIR_SET   (GPIO0_BASE + 0x04)
#define GPIO0_FUNC_SET  (GPIO0_BASE + 0x34)
#define GPIO0_WDATA_CLR (GPIO0_BASE + 0x48)

#define XRSTREQ_BIT     BIT(9)

void system_reset(void)
{
    __asm__ volatile("msr daifset, #0xf" ::: "memory");

    write32(GPIO0_DIR_SET,   XRSTREQ_BIT);  /* output                    */
    write32(GPIO0_WDATA_CLR, XRSTREQ_BIT);  /* preload low -- active low */
    write32(GPIO0_FUNC_SET,  XRSTREQ_BIT);  /* pad to the GPIO block     */
    dsb();

    for (;;)
        __asm__ volatile("wfi");
}
