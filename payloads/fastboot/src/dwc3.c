#include "dwc3.h"
#include "dwc3_regs.h"
#include "usb_phy.h"
#include "io.h"
#include "timer.h"

/*
 * DWC3 core bring-up for USB 2.0 high-speed device mode.
 *
 * Transcribed from drivers/usb/dwc3/core.c -- dwc3_core_init() at :851,
 * dwc3_phy_setup() at :594, dwc3_core_setup_global_control() at :758,
 * dwc3_event_buffers_setup() at :448 -- with everything SuperSpeed-only or
 * Linux-infrastructure-only removed.
 *
 * Two settings in the same file are Sony's rather than stock Synopsys, both
 * behind !CONFIG_ARCH_CXD900XX_FPGA: GCTL.PWRDNSCALE = 2 at :834 and the TCA
 * init at :880.
 */

/*
 * The DWC3 is an AXI master and DRAM is not trained in this context, so the
 * event buffer has to live in eSRAM -- hence the .usbdma linker region rather
 * than .bss.
 */
static u8 event_buffer[DWC3_EVENT_BUFFERS_SIZE]
    __attribute__((section(".usbdma"), aligned(64)));

static u32 core_revision;
static int tca_acked;
static int is_usb31;

u32 dwc3_revision(void) { return core_revision; }
int dwc3_tca_acked(void) { return tca_acked; }
int dwc3_is_usb31(void) { return is_usb31; }

static int dwc3_core_soft_reset(void)
{
    int retries = 1000;

    /* Device side only; we never run as host. */
    setbits32(DWC3_DCTL, DWC3_DCTL_CSFTRST);

    while (retries--) {
        if (!(read32(DWC3_DCTL) & DWC3_DCTL_CSFTRST)) {
            /* DWC_usb31 programming guide 1.3.2: at least 50 ms after
             * CSFTRST clears before the PHY domain may be accessed. */
            if (is_usb31)
                mdelay(50);
            return DWC3_OK;
        }
        udelay(1);
    }
    return DWC3_ERR_SOFTRESET;
}

static void dwc3_phy_setup(void)
{
    u32 reg;

    /* SUSPHY stays clear on both PHYs. dwc3_phy_setup() sets it for revisions
     * above 1.94a (drivers/usb/dwc3/core.c:614
     * and :700), but its own comment says the part
     * wants it clear until core initialisation has completed -- and a PHY that
     * suspends during bring-up stops the core responding. */
    clrbits32(DWC3_GUSB3PIPECTL(0), DWC3_GUSB3PIPECTL_SUSPHY);

    reg = read32(DWC3_GUSB2PHYCFG(0));

    /* UTMI+, 8-bit. arch/arm64/boot/dts/cxd/usb_ss.dtsi:57 says phy_type = "utmi" and notes
     * "0xc200 bit[3] = 0, ES: 8-bit MTMI+" [sic]. */
    reg &= ~DWC3_GUSB2PHYCFG_ULPI_UTMI;
    reg &= ~(DWC3_GUSB2PHYCFG_PHYIF_MASK | DWC3_GUSB2PHYCFG_USBTRDTIM_MASK);
    reg |= DWC3_GUSB2PHYCFG_PHYIF(UTMI_PHYIF_8_BIT);
    reg |= DWC3_GUSB2PHYCFG_USBTRDTIM(USBTRDTIM_UTMI_8_BIT);

    reg &= ~DWC3_GUSB2PHYCFG_SUSPHY;
    reg &= ~DWC3_GUSB2PHYCFG_ENBLSLPM;

    write32(DWC3_GUSB2PHYCFG(0), reg);
}

static void dwc3_setup_global_control(void)
{
    u32 reg = read32(DWC3_GCTL);

    reg &= ~DWC3_GCTL_SCALEDOWN_MASK;
    reg &= ~DWC3_GCTL_DISSCRAMBLE;
    reg &= ~DWC3_GCTL_DSBLCLKGTNG;

    /* Sony: power-down scale for this SoC's suspend clock. */
    reg &= ~DWC3_GCTL_PWRDNSCALE_MASK;
    reg |= DWC3_GCTL_PWRDNSCALE(2);

    write32(DWC3_GCTL, reg);
}

static void dwc3_event_buffer_setup(void)
{
    u64 addr = (u64)(unsigned long)event_buffer;
    u32 i;

    for (i = 0; i < sizeof(event_buffer); i++)
        event_buffer[i] = 0;

    write32(DWC3_GEVNTADRLO(0), (u32)addr);
    write32(DWC3_GEVNTADRHI(0), (u32)(addr >> 32));

    /* INTMASK: events still fill the buffer, but raise no interrupt. */
    write32(DWC3_GEVNTSIZ(0),
            DWC3_GEVNTSIZ_SIZE(sizeof(event_buffer)) | DWC3_GEVNTSIZ_INTMASK);
    write32(DWC3_GEVNTCOUNT(0), 0);

    dma_wmb();
}

static void dwc3_set_mode_device(void)
{
    clrsetbits32(DWC3_GCTL, DWC3_GCTL_PRTCAP_MASK,
                 DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_DEVICE));
}

int dwc3_core_init(void)
{
    u32 id;
    int ret;

    id = read32(DWC3_GSNPSID);
    if ((id & DWC3_GSNPSID_MASK) == DWC3_GSNPSID_DWC3) {
        is_usb31 = 0;
        core_revision = id & DWC3_GSNPSID_REVISION_MASK;
    } else if ((id & DWC3_GSNPSID_MASK) == DWC3_GSNPSID_DWC31) {
        is_usb31 = 1;
        core_revision = read32(DWC3_VER_NUMBER);
    } else {
        return DWC3_ERR_NOT_DWC3;
    }

    /* Before the soft reset, as dwc3_core_init() does at
     * drivers/usb/dwc3/core.c:882. The ACK
     * is recorded, not required. */
    tca_acked = (usb_phy_tca_init() == USB_PHY_OK);

    ret = dwc3_core_soft_reset();
    if (ret)
        return ret;

    dwc3_phy_setup();
    dwc3_setup_global_control();
    dwc3_set_mode_device();

    /* High speed, address 0. SuperSpeed is disabled in the glue. */
    clrsetbits32(DWC3_DCFG, DWC3_DCFG_SPEED_MASK | DWC3_DCFG_DEVADDR_MASK,
                 DWC3_DCFG_HIGHSPEED);

    dwc3_event_buffer_setup();

    /*
     * DEVTEN gates whether device events are written to the buffer at all; it
     * is not an interrupt mask. Only the events acted on are enabled:
     * dwc3_gadget_enable_irq() also enables EOPF
     * (drivers/usb/dwc3/gadget.c:1990), which fires
     * every microframe, and nothing drains the 4 KB buffer for the up-to-200
     * ms a dwc3_depcmd() can take.
     */
    write32(DWC3_DEVTEN,
            DWC3_DEVTEN_DISCONNEVTEN |
            DWC3_DEVTEN_USBRSTEN |
            DWC3_DEVTEN_CONNECTDONEEN);

    return DWC3_OK;
}

int dwc3_connect(void)
{
    u32 reg;
    int timeout = 500;

    dma_wmb();

    reg = read32(DWC3_DCTL);
    reg &= ~DWC3_DCTL_KEEP_CONNECT;
    reg |= DWC3_DCTL_RUN_STOP;
    write32(DWC3_DCTL, reg);

    /* Wait for the controller to leave the halted state. */
    while (timeout--) {
        if (!(read32(DWC3_DSTS) & DWC3_DSTS_DEVCTRLHLT))
            return DWC3_OK;
        udelay(100);
    }
    return DWC3_ERR_NOT_STARTED;
}

void dwc3_disconnect(void)
{
    clrbits32(DWC3_DCTL, DWC3_DCTL_RUN_STOP);
}

/* ---- endpoint / command layer ------------------------------------------- */

/* 200 ms; endpoint commands complete in microseconds when they work at all. */
#define DEPCMD_TIMEOUT_TICKS  (TIMER0_HZ / 5)

int dwc3_depcmd(u32 phys_ep, u32 cmd, u32 p0, u32 p1, u32 p2)
{
    u64 base = DWC3_DEP_BASE(phys_ep);
    u32 start, iters;
    u32 reg;

    write32(base + DWC3_DEPCMDPAR0, p0);
    write32(base + DWC3_DEPCMDPAR1, p1);
    write32(base + DWC3_DEPCMDPAR2, p2);
    dma_wmb();

    write32(base + DWC3_DEPCMD, cmd | DWC3_DEPCMD_CMDACT);

    /* Spin on the register rather than udelay()-ing between polls, so a stuck
     * command does not also freeze the activity indicator. */
    start = timer_ticks();
    iters = 0;
    for (;;) {
        reg = read32(base + DWC3_DEPCMD);
        if (!(reg & DWC3_DEPCMD_CMDACT))
            return (int)DWC3_DEPCMD_STATUS(reg);

        dwc3_wait_tick();

        /* The iteration count is the backstop for a stopped timer. */
        if (++iters > 4000000u)
            return -1;
        if (timer_ticks() - start > DEPCMD_TIMEOUT_TICKS)
            return -1;
    }
}

int dwc3_ep_start_config(void)
{
    return dwc3_depcmd(0, DWC3_DEPCMD_DEPSTARTCFG, 0, 0, 0);
}

int dwc3_ep_config(u32 phys_ep, u32 type, u32 maxpacket)
{
    u32 p0, p1;
    int ret;

    p0 = DWC3_DEPCFG_EP_TYPE(type)
       | DWC3_DEPCFG_MAX_PACKET_SIZE(maxpacket)
       | DWC3_DEPCFG_ACTION_INIT;

    /* Every IN endpoint needs a TxFIFO, numbered phys_ep >> 1. OUT endpoints
     * share the single RxFIFO and the field is ignored. */
    if (phys_ep & 1)
        p0 |= DWC3_DEPCFG_FIFO_NUMBER(phys_ep >> 1);

    /* EP_NUMBER is the PHYSICAL endpoint number. */
    p1 = DWC3_DEPCFG_EP_NUMBER(phys_ep) | DWC3_DEPCFG_INT_NUM(0);

    if (type == DWC3_DEPCMD_TYPE_CONTROL) {
        /* ep0 is driven by XferNotReady telling us which phase is wanted. */
        p1 |= DWC3_DEPCFG_XFER_COMPLETE_EN | DWC3_DEPCFG_XFER_NOT_READY_EN;
    } else {
        /* Single-TRB transfers with IOC|LST; InProgress matches what
         * dwc3_gadget_set_ep_config() enables for a non-control endpoint
         * (drivers/usb/dwc3/gadget.c:602). */
        p1 |= DWC3_DEPCFG_XFER_COMPLETE_EN | DWC3_DEPCFG_XFER_IN_PROGRESS_EN;
    }

    /*
     * SETTRANSFRESOURCE per endpoint, at configure time, rather than the batch
     * over all endpoints that dwc3_gadget_start_config() issues after
     * DEPSTARTCFG
     * (drivers/usb/dwc3/gadget.c:540): a resource handed to an endpoint still
     * absent from DALEPENA stops ep0 being delivered SETUPs.
     */
    ret = dwc3_depcmd(phys_ep, DWC3_DEPCMD_SETEPCONFIG, p0, p1, 0);
    if (ret)
        return ret;

    return dwc3_depcmd(phys_ep, DWC3_DEPCMD_SETTRANSFRESOURCE, 1, 0, 0);
}

void dwc3_ep_enable(u32 phys_ep)
{
    setbits32(DWC3_DALEPENA, DWC3_DALEPENA_EP(phys_ep));
}

void dwc3_ep_disable(u32 phys_ep)
{
    clrbits32(DWC3_DALEPENA, DWC3_DALEPENA_EP(phys_ep));
}

int dwc3_ep_start_xfer(u32 phys_ep, u64 trb_addr, u32 *rsc_idx)
{
    int ret;

    dma_wmb();
    ret = dwc3_depcmd(phys_ep, DWC3_DEPCMD_STARTTRANSFER,
                      (u32)(trb_addr >> 32), (u32)trb_addr, 0);
    if (ret)
        return ret;

    if (rsc_idx)
        *rsc_idx = DWC3_DEPCMD_GET_RSC_IDX(
                       read32(DWC3_DEP_BASE(phys_ep) + DWC3_DEPCMD));
    return 0;
}

int dwc3_ep_end_xfer(u32 phys_ep, u32 rsc_idx)
{
    return dwc3_depcmd(phys_ep,
                       DWC3_DEPCMD_ENDTRANSFER | DWC3_DEPCMD_HIPRI_FORCERM |
                       DWC3_DEPCMD_CMDIOC | DWC3_DEPCMD_PARAM(rsc_idx),
                       0, 0, 0);
}

int dwc3_ep_set_stall(u32 phys_ep)
{
    return dwc3_depcmd(phys_ep, DWC3_DEPCMD_SETSTALL, 0, 0, 0);
}

int dwc3_ep_clear_stall(u32 phys_ep)
{
    return dwc3_depcmd(phys_ep, DWC3_DEPCMD_CLEARSTALL, 0, 0, 0);
}

void dwc3_set_address(u32 addr)
{
    clrsetbits32(DWC3_DCFG, DWC3_DCFG_DEVADDR_MASK, DWC3_DCFG_DEVADDR(addr));
}

/*
 * GEVNTCOUNT is a byte count. The buffer is a hardware ring, so the read
 * position wraps independently of it.
 */
static u32 evt_lpos;

u32 dwc3_event_poll(void)
{
    u32 count, event;

    count = read32(DWC3_GEVNTCOUNT(0)) & DWC3_GEVNTCOUNT_MASK;
    if (count < 4)
        return 0;

    dsb();
    event = *(volatile u32 *)&event_buffer[evt_lpos];

    evt_lpos = (evt_lpos + 4) % sizeof(event_buffer);
    write32(DWC3_GEVNTCOUNT(0), 4);

    return event;
}
