#include "usb_phy.h"
#include "scu.h"
#include "io.h"

/*
 * USB 2.0 high-speed device bring-up.
 *
 * This is the whole of it. The sequence mirrors dwc3_of_cxd_usbphy_init() in
 * drivers/usb/dwc3/dwc3-of-cxd.c, minus everything that exists only to serve
 * SuperSpeed:
 *
 *   - download_snps_u3_phy_fw(), and the 4102-line firmware blob in
 *     u3_phy_fw_100a_cust1_patch3.h
 *   - the CR-port dance (enable_cr_para_sel / check_cr_sram_init_done /
 *     cr_para_sel_done) that exists solely to load that firmware into the SS
 *     PHY's SRAM
 *   - cr_phy31_adjust() and the Synopsys RX-low-Z workaround
 *
 * Skipping the CR port means leaving U31PHY_CFGR0.SRAM_BYPASS at its reset
 * default (set), which is the Synopsys "run from built-in defaults" boot. We
 * then set U31CTRL_CFGR0.u3_disable so the core never tries to use the SS pipe
 * at all, which is what force_u31_to_hs() does upstream.
 *
 * Confirmed starting state, read off a live camera with USB idle:
 *   SCU clock status 0xF1388000 = 0x00000FC0  -> USB gates (bits 12..15) OFF
 *   SCU reset status 0xF1388300 = 0x01001FF0  -> USB resets (bits 8..11) ASSERTED
 * so we cannot assume anything is already running.
 */

int usb_phy_init(void)
{
    /*
     * 1. Ungate the four USB clocks: ref (fix_sys_in), pclk (pll3/99),
     *    aclk (pll3/396), sus (pll4/33k).
     */
    write32(SCU_CLK_SET, CLK_USB_ALL);
    dsb();

    /*
     * 2. Assert all four USB resets. They already are on a cold boot, but
     *    doing it unconditionally makes this safe to re-run -- which matters
     *    when iterating with no debug output.
     *
     *    The 11 us matches the udelay(11) upstream; the comment there calls it
     *    "doing better than cure".
     */
    write32(SCU_RST_ASSERT, RST_USB_ALL);
    dsb();
    udelay(11);

    /*
     * 3. Release APB only. The PHY config registers below live behind the APB
     *    bus, so they have to be reachable before the PHY comes out of reset --
     *    the settings must be latched while the PHY is still held.
     */
    write32(SCU_RST_DEASSERT, RST_USB_APB);
    dsb();
    udelay(11);

    /*
     * 4. PHY reference clock from the pad (setup_cxd_u31phy(): clear bit 0).
     */
    clrbits32(U31PHY_CFGR0, U31PHY_CFGR0_PHY_REF);

    /*
     * 5. Disable USB 3.x entirely (force_u31_to_hs(): set u3_disable). The
     *    controller then advertises only high speed and never brings up the
     *    PIPE interface, so the unpatched SS PHY never matters.
     */
    setbits32(U31CTRL_CFGR0, U31CTRL_CFGR0_U3_DISABLE);
    dsb();

    /*
     * 6. Release the remaining resets: AXI, U2PHY, U31PHY.
     *
     *    U31PHY is released even though SuperSpeed is disabled. Upstream always
     *    releases it, and leaving a block held in reset while its clocks run is
     *    a good way to find out the hard way that some shared bit of the
     *    subsystem depended on it.
     */
    write32(SCU_RST_DEASSERT, RST_USB_AXI | RST_USB_U2PHY | RST_USB_U31PHY);
    dsb();

    /*
     * 7. Let the PHY settle. Upstream has a 100 ms wait in setup_cxd_uphy()
     *    with the comment "If not wait 100ms, the USB connection will fail.
     *    wait time are under investigation." That wait is in the TCA path
     *    rather than here, but it is cheap insurance and we are not in a hurry.
     */
    mdelay(100);

    return USB_PHY_OK;
}

/*
 * TCA is the Type-C alt-mode controller: it drives the mux that steers the SS
 * lanes and the connector orientation. For the Multi (micro) connector on a
 * USB-2.0-only link there should be nothing for it to do, so this is NOT called
 * from usb_phy_init(). Kept because it is the first thing to try if the host
 * never sees a pull-up.
 *
 * Transcribed from setup_cxd_uphy() in cxd-phy.c.
 */
int usb_phy_tca_init(void)
{
    u32 value;
    u32 retries = 1000;

    /* Clear then enable the TCA interrupt sources we poll on below. */
    setbits32(TCA_INTR_STS, 0xffffffff);
    setbits32(TCA_INTR_EN, TCA_FLD_ACK_EN | TCA_FLD_TIMEOUT_EN);

    mdelay(100);   /* upstream workaround, see above */

    value = read32(TCA_TCPC);
    value &= ~TCA_TCPC_MUX_MASK;
    value |= TCA_TCPC_MUX_CTL_USB31;
    value &= ~TCA_TCPC_CONNECTOR_ORIENTATION;   /* orientation 0 */
    value &= ~TCA_TCPC_LOW_POWER_EN;
    value |= TCA_TCPC_VALID;
    write32(TCA_TCPC, value);

    while (retries--) {
        if (read32(TCA_INTR_STS) & TCA_INTR_STS_ACK_EVT)
            goto acked;
        udelay(100);
    }
    return USB_PHY_ERR_TCA_TIMEOUT;

acked:
    /* SuperSpeed is disabled, so block SS operation explicitly. */
    setbits32(TCA_CTRLSYNCMODE_CFG0, TCA_CTRLSYNCMODE_CFG0_BLOCK_SS_OP);
    return USB_PHY_OK;
}
