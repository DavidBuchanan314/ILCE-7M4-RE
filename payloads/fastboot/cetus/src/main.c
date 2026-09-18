#include "io.h"
#include "mbox.h"
#include "ospi.h"
#include "monitor.h"
#include "uart.h"

volatile struct mbox mbox __attribute__((section(".mbox")));

int main(void)
{
    uart_init(0);
    uart_puts("[cetus] hello uart!\n");

    mbox.magic = MBOX_MAGIC;
    mbox.version = MBOX_VERSION;
    mbox.status = ST_RUNNING;
    mbox_step(STEP_ENTERED);

    /*
     * The flash is brought up once, here, rather than per command: the ROM's
     * sequence has to run before the part answers anything, and repeating it
     * on every request would make each one cost the settling delay.
     */
    if (ospi_init() != 0) {
        mbox.status = ST_TIMEOUT;
        mbox_step(STEP_HALTED);
    } else {
        mbox.status = ST_OK;
        mbox_step(STEP_READY);
    }

    /*
     * Answer on the same link the monitor used. Entering this payload took
     * the ROM's monitor away; this puts an equivalent one back, so the AP
     * never loses the CP and no reset is needed to collect results.
     */
    monitor_loop();
}
