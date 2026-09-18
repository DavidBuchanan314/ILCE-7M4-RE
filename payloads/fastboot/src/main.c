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

/*
 * Milestone: get the D+ pullup up, and report precisely how far we got.
 *
 * ep0 is not implemented, so enumeration is expected to FAIL. A host that sees
 * a device appear and then errors out reading its descriptor is the success
 * case here -- it proves clocks, reset release, PHY setup and the core are all
 * working. Host-side silence is the failure.
 *
 * The LED reports one number, held forever, identifying the furthest point
 * reached or the first thing that went wrong. Timings are deliberately slow:
 * a 1.5 s marker flash, 1.5 s dark, then N one-second blips, then 3 s dark.
 *
 *   3   fully up: core initialised, TCA acked, controller left the halted
 *       state, D+ pullup asserted
 *   4   same as 3, except TCA never acknowledged
 *   5   RUN_STOP written but DSTS.DEVCTRLHLT never cleared -- the controller
 *       did not actually start
 *   6   an ep0 endpoint command (DEPSTARTCFG/DEPCFG/DEPXFERCFG) was rejected
 *   7   GSNPSID was not 0x5533xxxx -- not talking to the core at all
 *   8   core soft reset timed out (DCTL.CSFTRST never cleared)
 *
 * If nothing at all happens after the three 1 s selftest flashes, execution
 * hung -- most likely an APB access to a block still held in reset.
 */

/*
 * USB comes up after the CP link, so anything that fails here has the UART to
 * say so on and does not need a blink code. A solid LED just marks the halt as
 * deliberate rather than a payload that never started.
 */
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

    /*
     * One short flash, not the six-second selftest that used to run here.
     * Everything the selftest confirmed -- that the payload executes, that
     * the timer runs near the rate timer.h claims -- the log line below says
     * better and in text, and it said it six seconds into every boot.
     *
     * The blip is still worth its 120 ms: it is the only sign of life for the
     * window before the CP is up, which is exactly where a hang would leave
     * nothing else to look at.
     */
    led_blip();

    /* Idle the Darwin chip select. Sends nothing -- the link is only used
     * once a command asks for it. */
    darwin_init();

    /*
     * Take the CP and put our own payload on it, before anything else that
     * could fail.
     *
     * Left alone it boots from NOR and its firmware reconfigures the flash,
     * and none of that is undone by a later reset. Resetting it into the ROM
     * monitor and immediately replacing that with the bundled payload means
     * the flash commands are available from the first command, with nothing
     * to stage by hand -- and, since that payload owns the UART, it is what
     * turns every line below from an LED blink count into text.
     *
     * A failure is not fatal: USB is the more useful of the two, and it is
     * also the only way to find out what went wrong. `oem cetus` reports it.
     */
    ret = cetus_bring_up();
    log_init();
    if (ret == 0)
        mira_log("cetus payload up");
    else
        mira_logx("cetus bring-up failed, rc", (u64)(u32)-ret);

    /*
     * Bring the eMMC up before USB. It is quick when it works (a handful of
     * commands, tens of milliseconds) and doing it here means the card is
     * ready by the time any command arrives.
     *
     * A failure is deliberately NOT fatal: USB is the more important of the
     * two, and losing it would also lose the channel needed to diagnose the
     * eMMC. `oem mmcinit` retries, and eMMC commands retry on demand.
     */
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

    /*
     * ep0 has to be configured and a SETUP armed BEFORE the pullup goes up,
     * or the host will reset the bus and send its first SETUP into a device
     * that is not listening yet.
     */
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

    /* Does not return. */
    usb_gadget_run();
}
