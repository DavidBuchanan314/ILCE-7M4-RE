#include "usb_phy.h"
#include "scu.h"
#include "io.h"

/*
 * USB 2.0 high-speed device bring-up, following dwc3_of_cxd_usbphy_init() in
 * drivers/usb/dwc3/dwc3-of-cxd.c:392, minus the SuperSpeed PHY firmware
 * download and the CR-port sequence that exists to load it.
 *
 * SRAM_BYPASS is left at its reset default (set), so the SS PHY runs from
 * built-in defaults, and u3_disable stops the core using the SS pipe at all.
 *
 * On a cold boot the USB clocks are gated and all four resets asserted.
 */

int usb_phy_init(void)
{
    /*
     * 1. Ungate the four USB clocks: ref (fix_sys_in), pclk (pll3/99),
     *    aclk (pll3/396), sus (pll4/33k).
     */
    write32(SCU_CLK_SET, CLK_USB_ALL);
    dsb();

    /* 2. Assert all four USB resets, so this is safe to re-run. */
    write32(SCU_RST_ASSERT, RST_USB_ALL);
    dsb();
    udelay(11);

    /*
     * 3. Release APB only: the PHY config registers below are behind it, and
     *    must be latched while the PHY is still held in reset.
     */
    write32(SCU_RST_DEASSERT, RST_USB_APB);
    dsb();
    udelay(11);

    /* 4. PHY reference clock from the pad (setup_cxd_u31phy(),
     *    drivers/usb/dwc3/cxd-phy.c:298:
     *    clear bit 0). */
    clrbits32(U31PHY_CFGR0, U31PHY_CFGR0_PHY_REF);

    /* 5. Disable USB 3.x (force_u31_to_hs(),
     *    drivers/usb/dwc3/cxd-phy.c:335), so the PIPE
     *    interface, and with it the unpatched SS PHY, never comes up. */
    setbits32(U31CTRL_CFGR0, U31CTRL_CFGR0_U3_DISABLE);
    dsb();

    /* 6. Release AXI, U2PHY and U31PHY. U31PHY too, as
     *    dwc3_of_cxd_usbphy_init() does, despite SuperSpeed being disabled. */
    write32(SCU_RST_DEASSERT, RST_USB_AXI | RST_USB_U2PHY | RST_USB_U31PHY);
    dsb();

    /* 7. setup_cxd_uphy() waits 100 ms here, with the comment at
     *    drivers/usb/dwc3/cxd-phy.c:84 "If not wait 100ms, the USB connection will fail." */
    mdelay(100);

    return USB_PHY_OK;
}

/*
 * TCA, the Type-C alt-mode controller, drives the SS lane mux and the
 * connector orientation. Transcribed from setup_cxd_uphy(),
 * drivers/usb/dwc3/cxd-phy.c:65,
 * which dwc3_core_init() calls unconditionally on non-FPGA
 * (drivers/usb/dwc3/core.c:880); this
 * payload does the same and only records the ACK.
 */
int usb_phy_tca_init(void)
{
    u32 value;
    u32 retries = 1000;

    /* Clear then enable the TCA interrupt sources we poll on below. */
    setbits32(TCA_INTR_STS, 0xffffffff);
    setbits32(TCA_INTR_EN, TCA_FLD_ACK_EN | TCA_FLD_TIMEOUT_EN);

    mdelay(100);   /* drivers/usb/dwc3/cxd-phy.c:86 */

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
