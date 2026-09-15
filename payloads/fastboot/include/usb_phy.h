#ifndef FASTBOOT_USB_PHY_H
#define FASTBOOT_USB_PHY_H

#include "io.h"

/*
 * USB subsystem glue. Two register windows sit outside the DWC3 core itself,
 * both named in usb_ss.dtsi:
 *
 *   reg = <0xF1080000 0x1000>,   reg-names = "ss_usb"   -- PHY/subsystem cfg
 *         <0xF10A0000 0x1C000>;               "phy_cr"  -- SS PHY CR port
 *
 * We only ever touch "ss_usb". The CR port exists to load a 16 KB firmware
 * patch into the SuperSpeed PHY's SRAM, and we do not bring SuperSpeed up.
 */
#define USB_SS_BASE         0xF1080000ull

#define U2PHY_CFGR0         (USB_SS_BASE + 0x0000)  /* crphy_defs.h */
#define U2PHY_CFGR1         (USB_SS_BASE + 0x0004)
#define U31CTRL_CFGR0       (USB_SS_BASE + 0x0010)  /* cxd-phy.c:319  */
#define U31PHY_CFGR0        (USB_SS_BASE + 0x0020)  /* cxd-phy.c:295  */

#define U31CTRL_CFGR0_U3_DISABLE    BIT(2)  /* force_u31_to_hs()   */
#define U31CTRL_CFGR0_GEN1          BIT(3)  /* force_u31_gen1()    */

#define U31PHY_CFGR0_PHY_REF        BIT(0)  /* 0 = ref clock from pad */
#define U31PHY_CFGR0_SRAM_BYPASS    BIT(16)
#define U31PHY_CFGR0_SRAM_EXT_LD_DONE BIT(17)
#define U31PHY_CFGR0_SRAM_INIT_DONE BIT(18)
#define U31PHY_CFGR0_PIPE_LANE0_RESET BIT(19)
#define U31PHY_CFGR0_CR_PARA_SEL    BIT(20)

/* TCA -- Type-C alt-mode controller, at DWC3 base + 0x10000. */
#define TCA_BASE            0xF10D0000ull
#define TCA_INTR_EN         (TCA_BASE + 0x04)
#define TCA_INTR_STS        (TCA_BASE + 0x08)
#define TCA_GCFG            (TCA_BASE + 0x10)
#define TCA_TCPC            (TCA_BASE + 0x14)
#define TCA_CTRLSYNCMODE_CFG0 (TCA_BASE + 0x20)
#define TCA_GEN_STATUS      (TCA_BASE + 0x34)

#define TCA_INTR_STS_ACK_EVT        BIT(0)
#define TCA_FLD_ACK_EN              BIT(0)
#define TCA_FLD_TIMEOUT_EN          BIT(1)
#define TCA_TCPC_MUX_MASK           0x3
#define TCA_TCPC_MUX_CTL_USB31      BIT(0)
#define TCA_TCPC_CONNECTOR_ORIENTATION BIT(2)
#define TCA_TCPC_LOW_POWER_EN       BIT(3)
#define TCA_TCPC_VALID              BIT(4)
#define TCA_CTRLSYNCMODE_CFG0_BLOCK_SS_OP BIT(0)

enum usb_phy_status {
    USB_PHY_OK              =  0,
    USB_PHY_ERR_TCA_TIMEOUT = -1,
};

/*
 * Bring the USB subsystem out of gate+reset and configure the USB 2.0 PHY for
 * high-speed device operation. Safe to call with USB in any state; the first
 * thing it does is assert all four resets.
 */
int usb_phy_init(void);

/* Optional TCA mux programming. Almost certainly unnecessary for the Multi
 * (micro) connector -- try without it first, and only add it if the host never
 * sees a pull-up. Separated out so it can be toggled independently. */
int usb_phy_tca_init(void);

#endif /* FASTBOOT_USB_PHY_H */
