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

    /* Once, not per command: the bring-up carries a settling delay. */
    if (ospi_init() != 0) {
        mbox.status = ST_TIMEOUT;
        mbox_step(STEP_HALTED);
    } else {
        mbox.status = ST_OK;
        mbox_step(STEP_READY);
    }

    /* Entering this payload took the ROM's monitor away; put an equivalent
     * one back on the same link. */
    monitor_loop();
}
