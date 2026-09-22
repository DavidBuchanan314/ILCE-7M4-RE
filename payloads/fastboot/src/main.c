#include "io.h"
#include "led.h"
#include "timer.h"
#include "usb_phy.h"
#include "dwc3.h"
#include "usb.h"
#include "sdhci.h"
#include "darwin.h"
#include "cetus.h"
#include "log.h"

/* The UART log has already said why. A solid LED marks the halt as
 * deliberate rather than a payload that never started. */
__attribute__((noreturn)) static void fatal(void)
{
    led_on();
    for (;;)
        ;
}

int main(void)
{
    int ret;

    led_init();
    timer_init();

    /* The only sign of life before the CP's UART is up. */
    led_blip();

    /* Idle the Darwin chip select. Sends nothing. */
    darwin_init();

    /*
     * First, because the CP's own firmware reconfigures the flash if it is
     * left to boot from NOR and a later reset does not undo that. It also owns
     * the UART every line below is logged on.
     *
     * Not fatal: USB is the only way to find out what went wrong, and
     * `oem cetus` reports it.
     */
    ret = cetus_bring_up();
    log_init();
    if (ret == 0)
        mira_log("cetus payload up");
    else
        mira_logx("cetus bring-up failed, rc", (u64)(u32)-ret);

    /* Not fatal: losing USB would also lose the channel needed to diagnose
     * this. `oem partition` retries. */
    mira_log("mmc init");
    if (mmc_init() == MMC_OK)
        mira_log("mmc ready");
    else
        mira_log("mmc init failed (not fatal)");

    mira_log("usb phy");
    usb_phy_init();

    ret = dwc3_core_init();
    if (ret != DWC3_OK) {
        mira_logx("dwc3 core init failed, rc", (u64)(u32)-ret);
        fatal();
    }
    mira_log("dwc3 core up");

    /* ep0 must be configured and a SETUP armed before the pullup goes up, or
     * the host's first SETUP lands on an endpoint that is not listening. */
    if (usb_gadget_init() != 0) {
        mira_log("ep0 config failed");
        fatal();
    }

    ret = dwc3_connect();
    if (ret != DWC3_OK) {
        mira_logx("dwc3 connect failed, rc", (u64)(u32)-ret);
        fatal();
    }

    mira_log("fastboot ready");

    usb_gadget_run();
}
