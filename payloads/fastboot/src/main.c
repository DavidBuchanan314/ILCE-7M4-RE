#include "io.h"
#include "led.h"
#include "timer.h"
#include "usb_phy.h"
#include "dwc3.h"
#include "usb.h"
#include "sdhci.h"
#include "darwin.h"
#include "cetus.h"

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

#define STATE_UP_TCA_OK     3
#define STATE_UP_TCA_FAIL   4
#define FAIL_EP0_CONFIG     6
#define FAIL_NOT_STARTED    5
#define FAIL_NOT_DWC3       7
#define FAIL_SOFTRESET      8

int main(void)
{
    int ret;

    led_init();
    timer_init();
    led_selftest();

    /*
     * Bring the eMMC up before USB. It is quick when it works (a handful of
     * commands, tens of milliseconds) and doing it here means the card is
     * ready by the time any command arrives.
     *
     * A failure is deliberately NOT fatal: USB is the more important of the
     * two, and losing it would also lose the channel needed to diagnose the
     * eMMC. `oem mmcinit` retries, and eMMC commands retry on demand.
     */
    mmc_init();

    /* Idle the Darwin chip select. Sends nothing -- the link is only used
     * once a command asks for it. */
    darwin_init();

    /*
     * Take the CP and put our own payload on it.
     *
     * Left alone it boots from NOR and its firmware reconfigures the flash,
     * and none of that is undone by a later reset. Resetting it into the ROM
     * monitor and immediately replacing that with the bundled payload means
     * the flash commands are available from the first command, with nothing
     * to stage by hand.
     *
     * A failure is not fatal -- USB is the more useful of the two, and it is
     * also the only way to find out what went wrong. `oem cetus` reports it.
     */
    cetus_bring_up();

    usb_phy_init();

    ret = dwc3_core_init();
    if (ret == DWC3_ERR_NOT_DWC3)
        led_fail(FAIL_NOT_DWC3);
    if (ret == DWC3_ERR_SOFTRESET)
        led_fail(FAIL_SOFTRESET);
    if (ret != DWC3_OK)
        led_panic();

    /*
     * ep0 has to be configured and a SETUP armed BEFORE the pullup goes up,
     * or the host will reset the bus and send its first SETUP into a device
     * that is not listening yet.
     */
    if (usb_gadget_init() != 0)
        led_fail(FAIL_EP0_CONFIG);

    ret = dwc3_connect();
    if (ret != DWC3_OK)
        led_fail(FAIL_NOT_STARTED);

    (void)STATE_UP_TCA_OK;
    (void)STATE_UP_TCA_FAIL;

    /* Does not return. */
    usb_gadget_run();
}
