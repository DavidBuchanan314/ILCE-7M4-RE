#ifndef FASTBOOT_DWC3_REGS_H
#define FASTBOOT_DWC3_REGS_H

#include "io.h"

/*
 * Synopsys DWC3 (USB 3.x dual-role controller), the stock IP -- not a Sony
 * part. Offsets below are transcribed from the camera's own kernel source,
 * drivers/usb/dwc3/core.h in Sony-ILCE-7M4-Linux, so they match this core
 * revision exactly.
 *
 * Base 0xF10C0000 from dt-bindings/soc/dwc3regs.h (USB_DWC3_REG_START, width
 * 0x11000). Linux's /proc/iomem shows the node at f10cc100 only because
 * dwc3 core.c advances the resource past the globals offset on probe.
 */
#define DWC3_BASE               0xF10C0000ull

#define DWC3_GLOBALS_REGS_START 0xc100
#define DWC3_DEVICE_REGS_START  0xc700
#define DWC3_PHY_OFFSET         0x10000   /* TCA block, 0xF10D0000 */

/* ---- global registers --------------------------------------------------- */
#define DWC3_GSBUSCFG0          (DWC3_BASE + 0xc100)
#define DWC3_GUCTL1             (DWC3_BASE + 0xc11c)
#define DWC3_GUCTL              (DWC3_BASE + 0xc12c)
#define DWC3_GUCTL2             (DWC3_BASE + 0xc19c)
#define DWC3_VER_NUMBER         (DWC3_BASE + 0xc1a0)
#define DWC3_GSBUSCFG1          (DWC3_BASE + 0xc104)
#define DWC3_GCTL               (DWC3_BASE + 0xc110)
#define DWC3_GSTS               (DWC3_BASE + 0xc118)
#define DWC3_GSNPSID            (DWC3_BASE + 0xc120)
#define DWC3_GUID               (DWC3_BASE + 0xc128)
#define DWC3_GHWPARAMS0         (DWC3_BASE + 0xc140)
#define DWC3_GHWPARAMS1         (DWC3_BASE + 0xc144)
#define DWC3_GHWPARAMS2         (DWC3_BASE + 0xc148)
#define DWC3_GHWPARAMS3         (DWC3_BASE + 0xc14c)
#define DWC3_GHWPARAMS4         (DWC3_BASE + 0xc150)
#define DWC3_GHWPARAMS5         (DWC3_BASE + 0xc154)
#define DWC3_GHWPARAMS6         (DWC3_BASE + 0xc158)
#define DWC3_GHWPARAMS7         (DWC3_BASE + 0xc15c)
#define DWC3_GHWPARAMS8         (DWC3_BASE + 0xc600)

#define DWC3_GUSB2PHYCFG(n)     (DWC3_BASE + 0xc200 + ((n) * 0x04))
#define DWC3_GUSB3PIPECTL(n)    (DWC3_BASE + 0xc2c0 + ((n) * 0x04))

#define DWC3_GEVNTADRLO(n)      (DWC3_BASE + 0xc400 + ((n) * 0x10))
#define DWC3_GEVNTADRHI(n)      (DWC3_BASE + 0xc404 + ((n) * 0x10))
#define DWC3_GEVNTSIZ(n)        (DWC3_BASE + 0xc408 + ((n) * 0x10))
#define DWC3_GEVNTCOUNT(n)      (DWC3_BASE + 0xc40c + ((n) * 0x10))

/* ---- device registers --------------------------------------------------- */
#define DWC3_DCFG               (DWC3_BASE + 0xc700)
#define DWC3_DCTL               (DWC3_BASE + 0xc704)
#define DWC3_DEVTEN             (DWC3_BASE + 0xc708)
#define DWC3_DSTS               (DWC3_BASE + 0xc70c)
#define DWC3_DALEPENA           (DWC3_BASE + 0xc720)

/* Per-endpoint command registers, physical ep number n. */
#define DWC3_DEP_BASE(n)        (DWC3_BASE + 0xc800 + ((n) * 0x10))
#define DWC3_DEPCMDPAR2         0x00
#define DWC3_DEPCMDPAR1         0x04
#define DWC3_DEPCMDPAR0         0x08
#define DWC3_DEPCMD             0x0c

/* ---- bit definitions ---------------------------------------------------- */
/*
 * Core identity. There are two families and they announce themselves
 * differently -- see dwc3_core_is_valid() upstream:
 *
 *   0x5533xxxx  "U3"  DWC_usb3,  revision in the low half of GSNPSID
 *   0x3331xxxx  "31"  DWC_usb31, revision in DWC3_VER_NUMBER instead
 *
 * The CXD90057 is the latter: usb_ssp.dtsi is titled "CXD90057 USB
 * SuperSpeedPlus" and declares maximum-speed = "super-speed-plus". Checking
 * only for "U3" rejects this core outright, which is exactly what an earlier
 * version of this file did.
 */
#define DWC3_GSNPSID_MASK           0xffff0000
#define DWC3_GSNPSID_DWC3           0x55330000  /* "U3" */
#define DWC3_GSNPSID_DWC31          0x33310000  /* "31" */
#define DWC3_GSNPSID_REVISION_MASK  0x0000ffff

#define DWC3_GCTL_PRTCAPDIR(n)      ((n) << 12)
#define DWC3_GCTL_PRTCAP_MASK       (3u << 12)
#define DWC3_GCTL_PRTCAP_HOST       1
#define DWC3_GCTL_PRTCAP_DEVICE     2
#define DWC3_GCTL_PRTCAP_OTG        3
#define DWC3_GCTL_CORESOFTRESET     BIT(11)
#define DWC3_GCTL_SCALEDOWN_MASK    (3u << 4)
#define DWC3_GCTL_DISSCRAMBLE       BIT(3)
#define DWC3_GCTL_DSBLCLKGTNG       BIT(0)
#define DWC3_GCTL_PWRDNSCALE(n)     ((n) << 19)
#define DWC3_GCTL_PWRDNSCALE_MASK   DWC3_GCTL_PWRDNSCALE(0x1fff)
#define DWC3_GCTL_U2EXIT_LFPS       BIT(2)
#define DWC3_GCTL_U2RSTECN          BIT(16)

#define DWC3_GUSB2PHYCFG_PHYSOFTRST BIT(31)
#define DWC3_GUSB2PHYCFG_SUSPHY     BIT(6)
#define DWC3_GUSB2PHYCFG_ULPI_UTMI  BIT(4)
#define DWC3_GUSB2PHYCFG_PHYIF(n)   ((n) << 3)
#define DWC3_GUSB2PHYCFG_ENBLSLPM   BIT(8)
#define DWC3_GUSB2PHYCFG_USBTRDTIM(n)    ((n) << 10)
#define DWC3_GUSB2PHYCFG_USBTRDTIM_MASK  DWC3_GUSB2PHYCFG_USBTRDTIM(0xf)
#define DWC3_GUSB2PHYCFG_PHYIF_MASK      DWC3_GUSB2PHYCFG_PHYIF(1)
#define DWC3_GUSB2PHYCFG_U2_FREECLK_EXISTS BIT(30)
#define USBTRDTIM_UTMI_8_BIT             9
#define UTMI_PHYIF_8_BIT                 0

#define DWC3_GUSB3PIPECTL_PHYSOFTRST BIT(31)
#define DWC3_GUSB3PIPECTL_SUSPHY    BIT(17)

#define DWC3_DCFG_DEVADDR(a)        ((a) << 3)
#define DWC3_DCFG_DEVADDR_MASK      DWC3_DCFG_DEVADDR(0x7f)
#define DWC3_DCFG_SPEED_MASK        7
#define DWC3_DCFG_HIGHSPEED         0
#define DWC3_DCFG_FULLSPEED         1
#define DWC3_DCFG_SUPERSPEED        4

#define DWC3_DEVTEN_VNDRDEVTSTRCVEDEN   BIT(12)
#define DWC3_DEVTEN_EVNTOVERFLOWEN      BIT(11)
#define DWC3_DEVTEN_CMDCMPLTEN          BIT(10)
#define DWC3_DEVTEN_ERRTICERREN         BIT(9)
#define DWC3_DEVTEN_SOFEN               BIT(7)
#define DWC3_DEVTEN_EOPFEN              BIT(6)
#define DWC3_DEVTEN_WKUPEVTEN           BIT(4)
#define DWC3_DEVTEN_ULSTCNGEN           BIT(3)
#define DWC3_DEVTEN_CONNECTDONEEN       BIT(2)
#define DWC3_DEVTEN_USBRSTEN            BIT(1)
#define DWC3_DEVTEN_DISCONNEVTEN        BIT(0)

#define DWC3_DCTL_RUN_STOP          BIT(31)
#define DWC3_DCTL_KEEP_CONNECT      BIT(19)
#define DWC3_DCTL_CSFTRST           BIT(30)

#define DWC3_DSTS_DEVCTRLHLT        BIT(22)
#define DWC3_DSTS_USBLNKST_MASK     (0x0f << 18)
#define DWC3_DSTS_USBLNKST(n)       (((n) & DWC3_DSTS_USBLNKST_MASK) >> 18)
#define DWC3_DSTS_CONNECTSPD        (7 << 0)
#define DWC3_DSTS_HIGHSPEED         (0 << 0)

#define DWC3_GEVNTCOUNT_MASK        0xfffc
#define DWC3_GEVNTCOUNT_EHB         BIT(31)
#define DWC3_GEVNTSIZ_INTMASK       BIT(31)
#define DWC3_GEVNTSIZ_SIZE(n)       ((n) & 0xffff)
#define DWC3_EVENT_BUFFERS_SIZE     4096

/* ---- endpoint commands -------------------------------------------------- */
#define DWC3_DEPCMD_PARAM_SHIFT     16
#define DWC3_DEPCMD_PARAM(x)        ((x) << DWC3_DEPCMD_PARAM_SHIFT)
#define DWC3_DEPCMD_GET_RSC_IDX(x)  (((x) >> DWC3_DEPCMD_PARAM_SHIFT) & 0x7f)
#define DWC3_DEPCMD_STATUS(x)       (((x) >> 12) & 0x0f)
#define DWC3_DEPCMD_CMDACT          BIT(10)
#define DWC3_DEPCMD_CMDIOC          BIT(8)

#define DWC3_DEPCMD_DEPSTARTCFG     0x09
#define DWC3_DEPCMD_ENDTRANSFER     0x08
#define DWC3_DEPCMD_STARTTRANSFER   0x06
#define DWC3_DEPCMD_CLEARSTALL      0x05
#define DWC3_DEPCMD_SETSTALL        0x04
#define DWC3_DEPCMD_SETTRANSFRESOURCE 0x02
#define DWC3_DEPCMD_SETEPCONFIG     0x01

#define DWC3_DEPCMD_TYPE_CONTROL    0
#define DWC3_DEPCMD_TYPE_ISOC       1
#define DWC3_DEPCMD_TYPE_BULK       2
#define DWC3_DEPCMD_TYPE_INTR       3

#define DWC3_DALEPENA_EP(n)         BIT(n)

/* DEPCFG parameter 0 */
#define DWC3_DEPCFG_EP_TYPE(n)          (((n) & 0x3) << 1)
#define DWC3_DEPCFG_MAX_PACKET_SIZE(n)  (((n) & 0x7ff) << 3)
#define DWC3_DEPCFG_FIFO_NUMBER(n)      (((n) & 0x1f) << 17)
#define DWC3_DEPCFG_BURST_SIZE(n)       (((n) & 0xf) << 22)
#define DWC3_DEPCFG_ACTION_INIT         (0u << 30)
#define DWC3_DEPCFG_ACTION_MODIFY       (2u << 30)

/* DEPCFG parameter 1 */
#define DWC3_DEPCFG_INT_NUM(n)          (((n) & 0x1f) << 0)
#define DWC3_DEPCFG_XFER_COMPLETE_EN    BIT(8)
#define DWC3_DEPCFG_XFER_IN_PROGRESS_EN BIT(9)
#define DWC3_DEPCFG_XFER_NOT_READY_EN   BIT(10)
#define DWC3_DEPCFG_EP_NUMBER(n)        (((n) & 0x1f) << 25)

/* ---- TRBs --------------------------------------------------------------- */
struct dwc3_trb {
    u32 bpl;    /* buffer pointer low  */
    u32 bph;    /* buffer pointer high */
    u32 size;
    u32 ctrl;
};

#define DWC3_TRB_SIZE_MASK          0x00ffffff
#define DWC3_TRB_SIZE_LENGTH(n)     ((n) & DWC3_TRB_SIZE_MASK)
#define DWC3_TRB_SIZE_TRBSTS(n)     (((n) & (0x0fu << 28)) >> 28)

#define DWC3_TRB_CTRL_HWO           BIT(0)
#define DWC3_TRB_CTRL_LST           BIT(1)
#define DWC3_TRB_CTRL_CHN           BIT(2)
#define DWC3_TRB_CTRL_CSP           BIT(3)
#define DWC3_TRB_CTRL_TRBCTL(n)     (((n) & 0x3f) << 4)
#define DWC3_TRB_CTRL_ISP_IMI       BIT(10)
#define DWC3_TRB_CTRL_IOC           BIT(11)

#define DWC3_TRBCTL_NORMAL          DWC3_TRB_CTRL_TRBCTL(1)
#define DWC3_TRBCTL_CONTROL_SETUP   DWC3_TRB_CTRL_TRBCTL(2)
#define DWC3_TRBCTL_CONTROL_STATUS2 DWC3_TRB_CTRL_TRBCTL(3)
#define DWC3_TRBCTL_CONTROL_STATUS3 DWC3_TRB_CTRL_TRBCTL(4)
#define DWC3_TRBCTL_CONTROL_DATA    DWC3_TRB_CTRL_TRBCTL(5)

/* ---- events ------------------------------------------------------------- */
/*
 * Events are single 32-bit words in the event buffer. Bit 0 discriminates:
 * 0 = endpoint event, 1 = device event. Layouts mirror struct
 * dwc3_event_depevt / dwc3_event_devt upstream, decoded by hand here so the
 * payload does not depend on bitfield layout rules.
 */
#define DWC3_EVENT_IS_DEVT(e)       ((e) & 1u)

#define DEPEVT_EP(e)                (((e) >> 1) & 0x1f)   /* physical ep */
#define DEPEVT_TYPE(e)              (((e) >> 6) & 0x0f)
#define DEPEVT_STATUS(e)            (((e) >> 12) & 0x0f)
#define DEPEVT_PARAM(e)             (((e) >> 16) & 0xffff)

#define DWC3_DEPEVT_XFERCOMPLETE    0x01
#define DWC3_DEPEVT_XFERINPROGRESS  0x02
#define DWC3_DEPEVT_XFERNOTREADY    0x03
#define DWC3_DEPEVT_EPCMDCMPLT      0x07

/* Control-phase encoding inside a XferNotReady status on ep0 */
#define DEPEVT_STATUS_CONTROL_DATA      1
#define DEPEVT_STATUS_CONTROL_STATUS    2
#define DEPEVT_STATUS_CONTROL_PHASE(n)  ((n) & 3)

#define DEVT_TYPE(e)                (((e) >> 1) & 0x7f)
#define DEVT_INFO(e)                (((e) >> 16) & 0x1ff)

#define DWC3_DEVICE_EVENT_DISCONNECT            0
#define DWC3_DEVICE_EVENT_RESET                 1
#define DWC3_DEVICE_EVENT_CONNECT_DONE          2
#define DWC3_DEVICE_EVENT_LINK_STATUS_CHANGE    3
#define DWC3_DEVICE_EVENT_WAKEUP                4
#define DWC3_DEVICE_EVENT_EOPF                  6
#define DWC3_DEVICE_EVENT_SOF                   7
#define DWC3_DEVICE_EVENT_ERRATIC_ERROR         9
#define DWC3_DEVICE_EVENT_CMD_CMPL              10
#define DWC3_DEVICE_EVENT_OVERFLOW              11

/*
 * Physical endpoint numbering: ep0out = 0, ep0in = 1, then (n<<1)|dir.
 * "dir" is 1 for IN. This is the number DEPCMD and the event `ep` field use;
 * it is not the bEndpointAddress the host sees.
 */
#define DWC3_PHYS_EP(num, is_in)    (((num) << 1) | ((is_in) ? 1 : 0))

#endif /* FASTBOOT_DWC3_REGS_H */
