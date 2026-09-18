#ifndef FASTBOOT_SCU_H
#define FASTBOOT_SCU_H

/*
 * System Control Unit -- clock gates and resets.
 *
 * Base and layout from cxd90057.dtsi (scu@F1388000, clock-controller@0,
 * rst@300), drivers/clk/cxd/cxd-clk.h (SET_REG_OFS/CLR_REG_OFS) and
 * drivers/reset/reset-cxd900xx.c (bank * 16 + 0/4/8).
 *
 * Both controllers use the same three-register-per-bank pattern:
 *   +0x0  status  (readable; 1 = clock enabled / reset asserted)
 *   +0x4  set     (write-only, W1S)
 *   +0x8  clear   (write-only, W1C)
 *
 * Confirmed on hardware: reading +0x4/+0x8 returns 0 on both the SCU and the
 * GPIO block, consistent with write-only set/clear.
 */
#define SCU_BASE            0xF1388000ull

/* Clock gate bank 0. USB gates live here; see clk-cxd-900xx.c:98-101. */
#define SCU_CLK_STS         (SCU_BASE + 0x000)
#define SCU_CLK_SET         (SCU_BASE + 0x004)
#define SCU_CLK_CLR         (SCU_BASE + 0x008)

#define CLK_USB_REF_CLK     BIT(12)  /* fix_sys_in    */
#define CLK_USB_PCLK        BIT(13)  /* fix_pll3_99   */
#define CLK_USB_ACLK        BIT(14)  /* fix_pll3_396  */
#define CLK_USB_SUS_CLK     BIT(15)  /* fix_pll4_33k  */
#define CLK_USB_ALL         (CLK_USB_REF_CLK | CLK_USB_PCLK | \
                             CLK_USB_ACLK    | CLK_USB_SUS_CLK)

/* Reset bank 0 (reset ids 0..31). USB ids 8..11. */
#define SCU_RST_STS         (SCU_BASE + 0x300)
#define SCU_RST_ASSERT      (SCU_BASE + 0x304)
#define SCU_RST_DEASSERT    (SCU_BASE + 0x308)

#define RST_USB_AXI         BIT(8)
#define RST_USB_APB         BIT(9)
#define RST_USB_U2PHY       BIT(10)
#define RST_USB_U31PHY      BIT(11)
#define RST_USB_ALL         (RST_USB_AXI | RST_USB_APB | \
                             RST_USB_U2PHY | RST_USB_U31PHY)

/* Clock gate bank 0x50: UARTs, SPIs, I2Cs. SPI0 is bit 12. */
#define SCU_CLK_PERI_STS    (SCU_BASE + 0x050)
#define SCU_CLK_PERI_SET    (SCU_BASE + 0x054)
#define SCU_CLK_PERI_CLR    (SCU_BASE + 0x058)

#define CLK_SPI0            BIT(12)

/* Clock source select bank 0x230. SPI0 takes bits 1:0; 0 selects fix_pll4_48. */
#define SCU_CLKSEL_SPI_STS  (SCU_BASE + 0x230)
#define SCU_CLKSEL_SPI_SET  (SCU_BASE + 0x234)
#define SCU_CLKSEL_SPI_CLR  (SCU_BASE + 0x238)

#define CLKSEL_SPI0_MASK    0x3u

/* Reset bank 5 (reset ids 160..191). SPI0 ids 172, 173. */
#define SCU_RST5_STS        (SCU_BASE + 0x350)
#define SCU_RST5_ASSERT     (SCU_BASE + 0x354)
#define SCU_RST5_DEASSERT   (SCU_BASE + 0x358)

#define RST_SPI0_SSP        BIT(12)
#define RST_SPI0_APB        BIT(13)

/* Read-only boot-mode / strap register. Bit 0 doubles as BOOT_SERIAL_IN. */
#define SCU_BOOTMODE        (SCU_BASE + 0xF10)

/* The CP's reset and boot-source select. Both writable, unlike our own. */
#define SCU_CP_RESET        (SCU_BASE + 0xF1C)
#define SCU_CP_BOOTMODE     (SCU_BASE + 0xF20)

#endif /* FASTBOOT_SCU_H */
