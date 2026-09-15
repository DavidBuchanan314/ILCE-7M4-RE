#include "dwc3.h"
#include "dwc3_regs.h"
#include "usb_phy.h"
#include "io.h"
#include "timer.h"

/*
 * DWC3 core bring-up for USB 2.0 high-speed device mode.
 *
 * Transcribed from dwc3_core_init() / dwc3_phy_setup() /
 * dwc3_core_setup_global_control() / dwc3_event_buffers_setup() in the
 * camera's own kernel drop, with everything SuperSpeed-only or
 * Linux-infrastructure-only removed.
 *
 * Two settings in here are Sony's rather than stock Synopsys, and both come
 * straight out of dwc3_core_setup_global_control():
 *   - GCTL.PWRDNSCALE = 2, guarded by !CONFIG_ARCH_CXD900XX_FPGA
 *   - the TCA init, which dwc3_core_init() calls unconditionally on non-FPGA
 * The second one is why usb_phy_tca_init() is called from here rather than
 * left as an optional extra: upstream always runs it, including for the Multi
 * connector, so matching it is the low-risk choice while we are working blind.
 */

/*
 * Event buffer. The DWC3 is an AXI master and DRAM is not trained in this
 * context, so this has to live in eSRAM -- hence the dedicated .usbdma linker
 * region at 0xFE030000 rather than .bss.
 *
 * With the MMU off, data accesses are Device-nGnRnE (uncached, strongly
 * ordered), so no cache maintenance is needed against the core's view of this
 * memory. If the MMU ever turns out to be on, that changes.
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
            /*
             * Mandatory on DWC_usb31: "once DWC3_DCTL_CSFTRST bit is cleared,
             * we must wait at least 50ms before accessing the PHY domain
             * (synchronization delay). DWC_usb31 programming guide section
             * 1.3.2." Everything after this touches the PHY domain, so
             * skipping it is not survivable.
             */
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

    /*
     * SuperSpeed pipe. We are not using it (u3_disable is set in the glue),
     * but SUSPHY is deliberately left CLEAR here: letting the PHY suspend
     * during bring-up is a classic way to make the core stop responding, and
     * upstream only sets it after init completes. Costs power, buys
     * predictability. Same reasoning for the USB2 block below.
     */
    clrbits32(DWC3_GUSB3PIPECTL(0), DWC3_GUSB3PIPECTL_SUSPHY);

    reg = read32(DWC3_GUSB2PHYCFG(0));

    /* UTMI+, 8-bit. usb_ss.dtsi says phy_type = "utmi" and notes
     * "0xc200 bit[3] = 0, ES: 8-bit UTMI+". */
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

    /*
     * INTMASK set: we poll GEVNTCOUNT rather than taking interrupts. fastboot
     * is strictly request/response, so a single-threaded poll loop is
     * sufficient and it saves setting up the GIC entirely.
     */
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

    /* TCA first: upstream runs it before the core soft reset. Its ACK is
     * recorded rather than treated as fatal -- a timeout here is informative,
     * not necessarily a stopper, on a USB-2.0-only link. */
    tca_acked = (usb_phy_tca_init() == USB_PHY_OK);

    ret = dwc3_core_soft_reset();
    if (ret)
        return ret;

    dwc3_phy_setup();
    dwc3_setup_global_control();
    dwc3_set_mode_device();

    /*
     * High speed, device address 0. SuperSpeed is disabled in the glue, so
     * advertising anything higher would just produce a device that never
     * connects.
     */
    clrsetbits32(DWC3_DCFG, DWC3_DCFG_SPEED_MASK | DWC3_DCFG_DEVADDR_MASK,
                 DWC3_DCFG_HIGHSPEED);

    dwc3_event_buffer_setup();

    /*
     * DEVTEN gates whether device events are WRITTEN TO THE EVENT BUFFER at
     * all -- it is not merely an interrupt mask. dwc3_gadget_disable_irq()
     * upstream sets it to 0 precisely to stop events being reported.
     *
     * This was previously 0, on the mistaken theory that polling meant no
     * events needed enabling. The part that makes polling work is
     * GEVNTSIZ.INTMASK (set in dwc3_event_buffer_setup), which suppresses the
     * interrupt while still filling the buffer. With DEVTEN clear we never saw
     * USB RESET, so after the host reset the device at the end of enumeration
     * ep0 was never re-initialised and the device went permanently deaf --
     * while the event loop carried on looking perfectly healthy.
     *
     * Same set as dwc3_gadget_enable_irq(), minus Start/End of Frame.
     */
    /*
     * Only the events we actually act on.
     *
     * Upstream additionally enables EOPF and CMDCMPLT, but it services the
     * event buffer from an interrupt. We poll, and we stop polling entirely
     * while dwc3_depcmd() waits -- up to 200 ms. EOPF fires every microframe,
     * so enabling it can queue ~1600 events in that window against a 4 KB
     * buffer, overflowing it and wedging the controller. Keeping the set
     * minimal keeps the buffer shallow.
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

    /*
     * Spin on the register directly rather than udelay()-ing between polls:
     * udelay() itself spins on the timer, so a stuck command used to freeze
     * here with no output at all. Ticking the heartbeat each iteration keeps
     * the LED reporting which command is hung.
     */
    start = timer_ticks();
    iters = 0;
    for (;;) {
        reg = read32(base + DWC3_DEPCMD);
        if (!(reg & DWC3_DEPCMD_CMDACT))
            return (int)DWC3_DEPCMD_STATUS(reg);

        dwc3_wait_tick();

        /*
         * Two independent bounds. The timer one is the meaningful deadline,
         * but it is useless if the timer itself has stopped -- and a loop that
         * cannot exit takes the debug channel with it. The iteration count is
         * the backstop.
         */
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

int dwc3_set_xfer_resource_all(u32 count)
{
    u32 i;

    for (i = 0; i < count; i++) {
        int ret = dwc3_depcmd(i, DWC3_DEPCMD_SETTRANSFRESOURCE, 1, 0, 0);
        if (ret)
            return ret;
    }
    return 0;
}

int dwc3_ep_config(u32 phys_ep, u32 type, u32 maxpacket)
{
    u32 p0, p1;
    int ret;

    p0 = DWC3_DEPCFG_EP_TYPE(type)
       | DWC3_DEPCFG_MAX_PACKET_SIZE(maxpacket)
       | DWC3_DEPCFG_ACTION_INIT;

    /*
     * Burst size is SuperSpeed-only; at high speed the field stays zero.
     *
     * Every IN endpoint needs a TxFIFO, numbered phys_ep >> 1 -- so ep0in
     * (physical 1) uses FIFO 0 and bulk IN (physical 3) uses FIFO 1. OUT
     * endpoints share the single RxFIFO and the field is ignored.
     */
    if (phys_ep & 1)
        p0 |= DWC3_DEPCFG_FIFO_NUMBER(phys_ep >> 1);

    /*
     * EP_NUMBER is the PHYSICAL endpoint number. Passing a logical number
     * here makes two endpoints claim the same identity and the controller
     * simply stops responding -- which looks exactly like a dead ep0.
     */
    p1 = DWC3_DEPCFG_EP_NUMBER(phys_ep) | DWC3_DEPCFG_INT_NUM(0);

    if (type == DWC3_DEPCMD_TYPE_CONTROL) {
        /* ep0 is driven by XferNotReady telling us which phase is wanted. */
        p1 |= DWC3_DEPCFG_XFER_COMPLETE_EN | DWC3_DEPCFG_XFER_NOT_READY_EN;
    } else {
        /* Single-TRB bulk transfers with IOC|LST: completion is what we act
         * on, and InProgress matches what upstream enables for non-control. */
        p1 |= DWC3_DEPCFG_XFER_COMPLETE_EN | DWC3_DEPCFG_XFER_IN_PROGRESS_EN;
    }

    /*
     * DEPCFG then SETTRANSFRESOURCE, per endpoint, at the point the endpoint
     * is actually configured.
     *
     * Upstream instead issues SETTRANSFRESOURCE for every endpoint in one
     * batch from dwc3_gadget_start_config(), and this file briefly did the
     * same -- but that hands a transfer resource to endpoints 2 and 3 while
     * they are still unconfigured and absent from DALEPENA, and ep0 stopped
     * being delivered SETUPs from that point on. Upstream gets away with it
     * because it has allocated and initialised every hardware endpoint before
     * that loop runs; we have not.
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
 * Event buffer consumer.
 *
 * GEVNTCOUNT holds the number of BYTES of valid events. We read one 32-bit
 * event, then acknowledge exactly 4 bytes. The buffer is a hardware ring, so
 * the read position wraps independently of the count.
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
