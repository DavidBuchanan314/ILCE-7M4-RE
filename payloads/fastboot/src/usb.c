#include "usb.h"
#include "dwc3.h"
#include "dwc3_regs.h"
#include "led.h"
#include "fastboot.h"
#include "io.h"
#include "timer.h"

/*
 * Minimal USB 2.0 device: one configuration, one vendor interface, two bulk
 * endpoints. Enough to be a fastboot target.
 *
 * The ep0 state machine is driven by XferNotReady events rather than by
 * assuming the phase order, which is how dwc3_ep0.c upstream does it. The
 * controller tells us when it wants the data stage and when it wants the
 * status stage, and which physical endpoint to queue each on -- so we never
 * have to work out the direction ourselves.
 */

#define EP0_OUT     DWC3_PHYS_EP(0, 0)   /* 0 */
#define EP0_IN      DWC3_PHYS_EP(0, 1)   /* 1 */
#define EP_OUT      DWC3_PHYS_EP(1, 0)   /* 2 */
#define EP_IN       DWC3_PHYS_EP(1, 1)   /* 3 */

/* ---- DMA-visible objects ------------------------------------------------ */
/* All of this must be reachable by the DWC3's AXI master, hence .usbdma. */

#define DMA_SECTION __attribute__((section(".usbdma"), aligned(64)))

static volatile struct dwc3_trb ep0_trb  DMA_SECTION;
static volatile struct dwc3_trb bulk_trb[2] DMA_SECTION;
static u8 setup_buf[64]                  DMA_SECTION;   /* >= maxpacket */
static u8 ep0_buf[512]                   DMA_SECTION;

/* ---- descriptors -------------------------------------------------------- */

static const u8 device_desc[18] = {
    18, USB_DT_DEVICE,
    0x00, 0x02,             /* bcdUSB 2.00 */
    0xff, 0xff, 0xff,       /* vendor specific class/subclass/protocol */
    EP0_MAXPACKET,
    0xd1, 0x18,             /* idVendor  0x18d1 (Google) */
    0xe0, 0x4e,             /* idProduct 0x4ee0 (fastboot) */
    0x00, 0x01,             /* bcdDevice 1.00 */
    1, 2, 3,                /* iManufacturer, iProduct, iSerial */
    1,                      /* bNumConfigurations */
};

/* Fastboot's interface signature: class 0xff, subclass 0x42, protocol 0x03. */
static const u8 config_desc[32] = {
    /* configuration */
    9, USB_DT_CONFIG,
    32, 0,                  /* wTotalLength */
    1,                      /* bNumInterfaces */
    1,                      /* bConfigurationValue */
    0,                      /* iConfiguration */
    0x80,                   /* bus powered */
    0xfa,                   /* 500 mA */

    /* interface */
    9, USB_DT_INTERFACE,
    0,                      /* bInterfaceNumber */
    0,                      /* bAlternateSetting */
    2,                      /* bNumEndpoints */
    0xff, 0x42, 0x03,       /* fastboot */
    0,                      /* iInterface */

    /* bulk OUT */
    7, USB_DT_ENDPOINT,
    EP_BULK_OUT_ADDR,
    USB_ENDPOINT_XFER_BULK,
    (BULK_MAXPACKET & 0xff), (BULK_MAXPACKET >> 8),
    0,

    /* bulk IN */
    7, USB_DT_ENDPOINT,
    EP_BULK_IN_ADDR,
    USB_ENDPOINT_XFER_BULK,
    (BULK_MAXPACKET & 0xff), (BULK_MAXPACKET >> 8),
    0,
};

/* Required when bcdUSB >= 0x0200. We are high-speed-only, so the "other
 * speed" it describes is full speed with no endpoints worth listing. */
static const u8 qualifier_desc[10] = {
    10, USB_DT_DEVICE_QUALIFIER,
    0x00, 0x02,
    0xff, 0xff, 0xff,
    EP0_MAXPACKET,
    1,                      /* bNumConfigurations */
    0,                      /* reserved */
};

/*
 * String descriptors. bLength counts the header plus the UTF-16LE payload
 * and nothing else -- an earlier version padded these arrays with spare
 * 0,0 pairs, which silently appended NUL characters to each string.
 */
static const u8 str_langid[4] = { 4, USB_DT_STRING, 0x09, 0x04 };

/* "Sony" */
static const u8 str_vendor[10] = {
    10, USB_DT_STRING,
    0x53, 0x00, 0x6f, 0x00, 0x6e, 0x00, 0x79, 0x00,
};

/* "ILCE-7M4" */
static const u8 str_product[18] = {
    18, USB_DT_STRING,
    0x49, 0x00, 0x4c, 0x00, 0x43, 0x00, 0x45, 0x00, 0x2d, 0x00, 0x37, 0x00, 0x4d, 0x00, 0x34, 0x00,
};

/* "ILCE7M4-fastboot" */
static const u8 str_serial[34] = {
    34, USB_DT_STRING,
    0x49, 0x00, 0x4c, 0x00, 0x43, 0x00, 0x45, 0x00, 0x37, 0x00, 0x4d, 0x00, 0x34, 0x00, 0x2d, 0x00,
    0x66, 0x00, 0x61, 0x00, 0x73, 0x00, 0x74, 0x00, 0x62, 0x00, 0x6f, 0x00, 0x6f, 0x00, 0x74, 0x00,
};

/* ---- ep0 state ---------------------------------------------------------- */

/*
 * Which phase the single ep0 TRB is currently servicing. Tracked explicitly
 * rather than inferred from the TRB's control field: one TRB is reused for
 * setup, data and status, and the completion handler has to know which of the
 * three just finished in order to decide what comes next. Getting this wrong
 * means enumeration stops dead after the first control transfer, because
 * nothing re-arms the SETUP.
 */
enum ep0_phase {
    EP0_PHASE_SETUP,
    EP0_PHASE_DATA,
    EP0_PHASE_STATUS,
};

static enum ep0_phase ep0_phase;
static struct usb_ctrlrequest ctrl;

static volatile int bulk_out_busy, bulk_in_busy;
static volatile u32 bulk_out_residue, bulk_in_residue;

/*
 * Incremented on every bus reset. A reset makes ep0_init() issue DEPSTARTCFG
 * with param 0, which releases the transfer resources of EVERY endpoint --
 * silently killing any bulk transfer already queued. The host does this on
 * every fastboot command (it resets the device when claiming the interface),
 * so an outstanding bulk transfer being torn out from under us is the normal
 * case, not an error. The bulk helpers compare this across their wait loop and
 * re-queue when it moves.
 */
static volatile u32 bulk_epoch;

/* Set by SET_CONFIGURATION, acted on by usb_bulk_enable_pending(). */
static volatile int bulk_enable_pending;


static const u8 *ep0_data;      /* what to send in the data stage */
static u32 ep0_data_len;
static volatile int configured;

static u16 get_le16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }









static void ep0_queue_setup(void)
{
    ep0_trb.bpl  = (u32)(unsigned long)setup_buf;
    ep0_trb.bph  = (u32)((u64)(unsigned long)setup_buf >> 32);
    ep0_trb.size = 8;
    ep0_trb.ctrl = DWC3_TRBCTL_CONTROL_SETUP
                 | DWC3_TRB_CTRL_HWO
                 | DWC3_TRB_CTRL_LST
                 | DWC3_TRB_CTRL_IOC
                 | DWC3_TRB_CTRL_ISP_IMI;
    ep0_phase = EP0_PHASE_SETUP;
    dma_wmb();
    usb_state = dwc3_ep_start_xfer(EP0_OUT, (u64)(unsigned long)&ep0_trb, 0)
              ? EP0_TRACE_ARM_FAILED : EP0_TRACE_ARMED;
}

static void ep0_queue_data(u32 phys_ep, const void *buf, u32 len)
{
    u32 xfer = len;

    /*
     * An OUT transfer length must be a multiple of the endpoint's maximum
     * packet size, or the controller reports a babble/short error. Round up:
     * the buffer is large enough and the surplus is discarded.
     */
    if (!(phys_ep & 1))
        xfer = (len + EP0_MAXPACKET - 1) & ~(u32)(EP0_MAXPACKET - 1);

    ep0_trb.bpl  = (u32)(unsigned long)buf;
    ep0_trb.bph  = (u32)((u64)(unsigned long)buf >> 32);
    ep0_trb.size = xfer;
    ep0_trb.ctrl = DWC3_TRBCTL_CONTROL_DATA
                 | DWC3_TRB_CTRL_HWO
                 | DWC3_TRB_CTRL_LST
                 | DWC3_TRB_CTRL_IOC
                 | DWC3_TRB_CTRL_ISP_IMI;
    ep0_phase = EP0_PHASE_DATA;
    usb_state = EP0_TRACE_DATA_QUEUED;
    dma_wmb();
    dwc3_ep_start_xfer(phys_ep, (u64)(unsigned long)&ep0_trb, 0);
}

static void ep0_queue_status(u32 phys_ep, int three_stage)
{
    ep0_trb.bpl  = 0;
    ep0_trb.bph  = 0;
    ep0_trb.size = 0;
    ep0_trb.ctrl = (three_stage ? DWC3_TRBCTL_CONTROL_STATUS3
                                : DWC3_TRBCTL_CONTROL_STATUS2)
                 | DWC3_TRB_CTRL_HWO
                 | DWC3_TRB_CTRL_LST
                 | DWC3_TRB_CTRL_IOC
                 | DWC3_TRB_CTRL_ISP_IMI;
    ep0_phase = EP0_PHASE_STATUS;
    usb_state = EP0_TRACE_STATUS_QUEUED;
    dma_wmb();
    dwc3_ep_start_xfer(phys_ep, (u64)(unsigned long)&ep0_trb, 0);
}

static void ep0_stall(void)
{
    dwc3_ep_set_stall(EP0_OUT);
    ep0_queue_setup();
}

/* ---- bulk endpoints ----------------------------------------------------- */

static void bulk_endpoints_enable(void)
{
    int ret;

    /*
     * No DEPSTARTCFG here. Transfer resources were assigned once when ep0 came
     * up; re-running DEPSTARTCFG now would reset them and kill the control
     * endpoint mid-session. Enabling a non-ep0 endpoint is just DEPCFG plus
     * DALEPENA.
     */
    usb_state = USB_STATE_CFG_OUT_ISSUING;
    ret = dwc3_ep_config(EP_OUT, DWC3_DEPCMD_TYPE_BULK, BULK_MAXPACKET);
    if (ret) {
        usb_bulk_error = USB_BULK_ERR_CFG_OUT;
        usb_state = USB_STATE_CFG_OUT_FAILED;
        return;
    }

    usb_state = USB_STATE_CFG_IN_ISSUING;
    ret = dwc3_ep_config(EP_IN, DWC3_DEPCMD_TYPE_BULK, BULK_MAXPACKET);
    if (ret) {
        usb_bulk_error = USB_BULK_ERR_CFG_IN;
        usb_state = USB_STATE_CFG_IN_FAILED;
        return;
    }

    dwc3_ep_enable(EP_OUT);
    dwc3_ep_enable(EP_IN);
}

/* ---- standard request handling ------------------------------------------ */

static int handle_get_descriptor(void)
{
    u8 type  = (u8)(ctrl.wValue >> 8);
    u8 index = (u8)(ctrl.wValue & 0xff);

    switch (type) {
    case USB_DT_DEVICE:
        ep0_data = device_desc;
        ep0_data_len = sizeof(device_desc);
        return 0;
    case USB_DT_CONFIG:
        ep0_data = config_desc;
        ep0_data_len = sizeof(config_desc);
        return 0;
    case USB_DT_DEVICE_QUALIFIER:
        ep0_data = qualifier_desc;
        ep0_data_len = sizeof(qualifier_desc);
        return 0;
    case USB_DT_STRING:
        switch (index) {
        case 0: ep0_data = str_langid;  ep0_data_len = sizeof(str_langid);  return 0;
        case 1: ep0_data = str_vendor;  ep0_data_len = sizeof(str_vendor);  return 0;
        case 2: ep0_data = str_product; ep0_data_len = sizeof(str_product); return 0;
        case 3: ep0_data = str_serial;  ep0_data_len = sizeof(str_serial);  return 0;
        }
        return -1;
    }
    return -1;
}

static int handle_setup(void)
{
    ep0_data = 0;
    ep0_data_len = 0;

    if ((ctrl.bRequestType & USB_TYPE_MASK) != USB_TYPE_STANDARD)
        return -1;

    switch (ctrl.bRequest) {
    case USB_REQ_GET_DESCRIPTOR:
        if (handle_get_descriptor() < 0)
            return -1;
        if (ep0_data_len > ctrl.wLength)
            ep0_data_len = ctrl.wLength;
        return 0;

    case USB_REQ_SET_ADDRESS:
        /* DWC3 wants the address programmed now; it applies it after the
         * status stage itself. */
        dwc3_set_address(ctrl.wValue & 0x7f);
        return 0;

    case USB_REQ_SET_CONFIGURATION:
        if ((ctrl.wValue & 0xff) == 1) {
            /*
             * Do NOT configure the bulk endpoints here. This runs inside the
             * SETUP stage of the control transfer, and an endpoint command
             * that stalls the controller's command interface at this point
             * also prevents ep0 from arming its next SETUP -- taking the whole
             * device down, debug channel included. Defer it to the main loop,
             * after this transfer has finished cleanly.
             */
            configured = 1;
            bulk_enable_pending = 1;
            usb_state = USB_STATE_IN_SET_CONFIG;
            return 0;
        }
        if ((ctrl.wValue & 0xff) == 0) {
            configured = 0;
            return 0;
        }
        return -1;

    case USB_REQ_GET_CONFIGURATION:
        ep0_buf[0] = (u8)(configured ? 1 : 0);
        ep0_data = ep0_buf;
        ep0_data_len = 1;
        return 0;

    case USB_REQ_GET_STATUS:
        ep0_buf[0] = 0x01;   /* self powered */
        ep0_buf[1] = 0x00;
        ep0_data = ep0_buf;
        ep0_data_len = 2;
        if (ep0_data_len > ctrl.wLength)
            ep0_data_len = ctrl.wLength;
        return 0;

    case USB_REQ_SET_INTERFACE:
        return 0;

    case USB_REQ_GET_INTERFACE:
        ep0_buf[0] = 0;
        ep0_data = ep0_buf;
        ep0_data_len = 1;
        return 0;

    case USB_REQ_CLEAR_FEATURE:
    case USB_REQ_SET_FEATURE:
        return 0;
    }
    return -1;
}

/* ---- event handling ----------------------------------------------------- */

static int ep0_init(void)
{
    int ret;

    /* Once per bus reset: reset the controller's resource assignment. Each
     * endpoint then claims its own resource as part of being configured. */
    ret = dwc3_ep_start_config();
    if (ret)
        return ret;

    ret = dwc3_ep_config(EP0_OUT, DWC3_DEPCMD_TYPE_CONTROL, EP0_MAXPACKET);
    if (ret)
        return ret;

    ret = dwc3_ep_config(EP0_IN, DWC3_DEPCMD_TYPE_CONTROL, EP0_MAXPACKET);
    if (ret)
        return ret;

    dwc3_ep_enable(EP0_OUT);
    dwc3_ep_enable(EP0_IN);
    ep0_queue_setup();
    return 0;
}

static void on_reset(void)
{
    usb_state = USB_STATE_IN_RESET;
    configured = 0;

    /*
     * Drop the bulk endpoints before ep0_init() pulls their resources away,
     * and release anyone waiting on a transfer that is now void.
     */
    dwc3_ep_disable(EP_OUT);
    dwc3_ep_disable(EP_IN);
    bulk_out_busy = 0;
    bulk_in_busy = 0;
    bulk_epoch++;

    dwc3_set_address(0);

    /*
     * ep0_init()'s result used to be discarded here. If it fails after a bus
     * reset the control endpoint never re-arms a SETUP, so the device silently
     * stops answering while the event loop carries on looking healthy.
     */
    if (ep0_init() != 0)
        usb_state = USB_STATE_RESET_EP0_FAIL;
}

static void handle_depevt(u32 event)
{
    u32 ep     = DEPEVT_EP(event);
    u32 type   = DEPEVT_TYPE(event);
    u32 status = DEPEVT_STATUS(event);

    if (ep == EP_OUT || ep == EP_IN) {
        /*
         * Accept either completion flavour.
         *
         * With XFER_IN_PROGRESS_EN set, this core reports a finished TRB that
         * had IOC as XferInProgress rather than XferComplete -- which is why
         * dwc3_gadget.c routes DWC3_DEPEVT_XFERINPROGRESS into
         * dwc3_endpoint_transfer_complete() for non-control endpoints. Waiting
         * only on XferComplete spins forever on an event that never comes.
         * Handling both is correct regardless of which one the core picks.
         */
        if (type == DWC3_DEPEVT_XFERCOMPLETE ||
            type == DWC3_DEPEVT_XFERINPROGRESS) {
            /*
             * TRB.size holds the RESIDUE -- how much of the requested length
             * was not transferred -- so the actual count is requested minus
             * this. For a short OUT packet that is how we learn the real
             * length of a fastboot command.
             */
            if (ep == EP_OUT) {
                bulk_out_residue = bulk_trb[0].size & DWC3_TRB_SIZE_MASK;
                bulk_out_busy = 0;
            } else {
                bulk_in_residue = bulk_trb[1].size & DWC3_TRB_SIZE_MASK;
                bulk_in_busy = 0;
            }
        }
        return;
    }

    if (ep > EP0_IN)
        return;

    switch (type) {
    case DWC3_DEPEVT_XFERCOMPLETE:
        switch (ep0_phase) {
        case EP0_PHASE_SETUP:
            /* A new control request has landed in setup_buf. */
            dsb();
            ctrl.bRequestType = setup_buf[0];
            ctrl.bRequest     = setup_buf[1];
            ctrl.wValue       = get_le16(&setup_buf[2]);
            ctrl.wIndex       = get_le16(&setup_buf[4]);
            ctrl.wLength      = get_le16(&setup_buf[6]);

            usb_state = EP0_TRACE_GOT_SETUP;
            if (handle_setup() < 0)
                ep0_stall();
            /* Otherwise wait: the controller raises XferNotReady when it
             * wants the data or status stage. */
            break;

        case EP0_PHASE_DATA:
            /* Nothing to do -- a XferNotReady(STATUS) follows. */
            break;

        case EP0_PHASE_STATUS:
            /* Transfer finished. Re-arm so a SETUP is always outstanding. */
            ep0_queue_setup();
            break;
        }
        break;

    case DWC3_DEPEVT_XFERNOTREADY:
        switch (DEPEVT_STATUS_CONTROL_PHASE(status)) {
        case DEPEVT_STATUS_CONTROL_DATA:
            if (!ep0_data || !ep0_data_len) {
                /* Host wants data we do not have. */
                ep0_stall();
                break;
            }
            ep0_queue_data(ep, ep0_data, ep0_data_len);
            break;

        case DEPEVT_STATUS_CONTROL_STATUS:
            ep0_queue_status(ep, ctrl.wLength ? 1 : 0);
            break;
        }
        break;

    default:
        break;
    }
}

static void handle_devt(u32 event)
{
    switch (DEVT_TYPE(event)) {
    case DWC3_DEVICE_EVENT_RESET:
        on_reset();
        break;
    case DWC3_DEVICE_EVENT_CONNECT_DONE:
        /* Speed is fixed high by DCFG; nothing to renegotiate. Re-arm ep0
         * so a SETUP is always outstanding. */
        ep0_init();
        break;
    case DWC3_DEVICE_EVENT_DISCONNECT:
        configured = 0;
        break;
    default:
        break;
    }
}

int usb_gadget_init(void)
{
    return ep0_init();
}

/* Coarse "how far did we get" marker, reported by the LED heartbeat. */
volatile u32 usb_state = USB_STATE_UNCONFIGURED;

/* Lets the activity light go out while dwc3_depcmd() waits on the controller. */
void dwc3_wait_tick(void)
{
    led_activity_tick();
}

void usb_event_pump(void)
{
    u32 event;
    int budget = 64;

    led_activity_tick();

    /* Drain rather than taking one event per call: the buffer is only 4 KB and
     * nothing consumes it while an endpoint command is waiting. The budget
     * stops a flood from starving the heartbeat. */
    while (budget-- > 0) {
        event = dwc3_event_poll();
        if (!event)
            return;

        led_activity();

        if (DWC3_EVENT_IS_DEVT(event))
            handle_devt(event);
        else
            handle_depevt(event);
    }
}

int usb_is_configured(void)
{
    return configured;
}

/*
 * Perform any deferred bulk endpoint setup. Called from the main loop, outside
 * any control transfer, so a failure here cannot strand ep0 mid-transaction.
 */
void usb_bulk_enable_pending(void)
{
    if (!bulk_enable_pending)
        return;
    bulk_enable_pending = 0;
    usb_state = USB_STATE_ENABLING_BULK;
    bulk_endpoints_enable();
    if (!usb_bulk_error)
        usb_state = USB_STATE_CONFIGURED;
}

/* ---- bulk transfers ----------------------------------------------------- */

/* Set when an endpoint command during SET_CONFIGURATION was rejected, or when
 * a bulk transfer could not be started / never completed. Reported by
 * fastboot_loop() via the LED, because a wedged bulk path is otherwise
 * indistinguishable from a hang. */
int usb_bulk_error;

/* Resource index per direction, so a queued transfer can be ended later. */
static u32 bulk_rsc[2];

static int bulk_queue(u32 phys_ep, const void *buf, u32 len, int is_in)
{
    volatile struct dwc3_trb *trb = &bulk_trb[is_in ? 1 : 0];

    trb->bpl  = (u32)(unsigned long)buf;
    trb->bph  = (u32)((u64)(unsigned long)buf >> 32);
    trb->size = len;
    trb->ctrl = DWC3_TRBCTL_NORMAL
              | DWC3_TRB_CTRL_HWO
              | DWC3_TRB_CTRL_LST
              | DWC3_TRB_CTRL_IOC
              | DWC3_TRB_CTRL_ISP_IMI;
    dma_wmb();
    return dwc3_ep_start_xfer(phys_ep, (u64)(unsigned long)trb,
                              &bulk_rsc[is_in ? 1 : 0]);
}

/*
 * Applied ONLY to transfers the host has already committed to. Receiving is
 * not one of them: usb_bulk_recv() is how we wait for the host to send a
 * fastboot command, and that wait is open-ended -- the host may sit idle
 * indefinitely before anyone runs a command. Timing it out treated normal
 * idling as a failure and halted the device, so every later command NAKed
 * forever.
 *
 * Half a second is the deadline a host that is genuinely reading will never
 * come near -- a 64-byte reply lands in microseconds and even a full upload
 * chunk is a couple of milliseconds -- while being what a host that has gone
 * away costs us before the endpoint is reclaimed. The slack is for the host
 * stalling between reads of a long upload, not for the transfer itself.
 */
#define BULK_TIMEOUT_TICKS  (TIMER0_HZ / 2)

u32 usb_bulk_recv(void *buf, u32 len)
{
    /*
     * An OUT transfer length must be a whole number of maximum-size packets;
     * the controller rejects anything else. A short packet from the host ends
     * the transfer early and the residue tells us how much actually arrived.
     */
    u32 xfer = (len + BULK_MAXPACKET - 1) & ~(u32)(BULK_MAXPACKET - 1);

    for (;;) {
        u32 epoch;

        if (!configured) {
            /* Distinguish "never configured" from "reset, awaiting
             * re-configuration" -- they look identical otherwise. */
            if (usb_state != USB_STATE_RESET_EP0_FAIL)
                usb_state = USB_STATE_WAIT_RECONFIG;
            while (!configured)
                usb_event_pump();
        }

        epoch = bulk_epoch;
        bulk_out_residue = 0;
        bulk_out_busy = 1;
        usb_state = USB_STATE_AWAIT_CMD;

        if (bulk_queue(EP_OUT, buf, xfer, 0) != 0) {
            usb_bulk_error = USB_BULK_ERR_OUT_CMD;
            return 0;
        }

        /* No deadline: waiting for the host is the normal state. */
        usb_state = USB_STATE_BULK_WAIT_OUT;
        while (bulk_out_busy && bulk_epoch == epoch)
            usb_event_pump();

        /* Reset mid-flight: the transfer is void, wait to be reconfigured
         * and queue it again. */
        if (bulk_epoch != epoch)
            continue;

        dsb();
        usb_state = USB_STATE_GOT_CMD;
        return xfer - bulk_out_residue;
    }
}

/*
 * Cancel anything queued on the bulk endpoints.
 *
 * A transfer left queued does not stay harmless: it completes whenever the
 * host next reads, delivering a reply to a command nobody asked for, and from
 * then on every answer is one behind.
 */
void usb_bulk_abort(void)
{
    if (bulk_in_busy) {
        dwc3_ep_end_xfer(EP_IN, bulk_rsc[1]);
        bulk_in_busy = 0;
    }
    if (bulk_out_busy) {
        dwc3_ep_end_xfer(EP_OUT, bulk_rsc[0]);
        bulk_out_busy = 0;
    }
}

/*
 * A host that stops reading mid-reply -- interrupted, killed, or piped into
 * something that closed early -- is a client problem, not a device one, so it
 * costs one aborted transfer rather than the session.
 */
int usb_bulk_recover(void)
{
    if (usb_bulk_error != USB_BULK_ERR_IN_TIMEOUT)
        return -1;

    usb_bulk_abort();
    usb_bulk_error = 0;
    usb_state = USB_STATE_FB_LOOP_TOP;
    return 0;
}

u32 usb_bulk_send(const void *buf, u32 len)
{
    /* Once the host has stopped listening, the rest of this reply is going
     * nowhere; spending the full deadline per line would turn one dropped
     * response into minutes of stalling. */
    if (usb_bulk_error)
        return 0;

    for (;;) {
        u32 epoch, start;

        while (!configured)
            usb_event_pump();

        epoch = bulk_epoch;
        bulk_in_residue = 0;
        bulk_in_busy = 1;

        if (bulk_queue(EP_IN, buf, len, 1) != 0) {
            usb_bulk_error = USB_BULK_ERR_IN_CMD;
            return 0;
        }

        usb_state = USB_STATE_BULK_WAIT_IN;
        start = timer_ticks();
        while (bulk_in_busy && bulk_epoch == epoch) {
            usb_event_pump();
            if (timer_ticks() - start > BULK_TIMEOUT_TICKS) {
                usb_bulk_error = USB_BULK_ERR_IN_TIMEOUT;
                usb_bulk_abort();
                return 0;
            }
        }

        /*
         * A reset between receiving a command and answering it means the host
         * is no longer listening for that answer. Drop it rather than retry:
         * re-sending a stale reply desynchronises the command/response
         * lockstep the protocol depends on.
         */
        if (bulk_epoch != epoch)
            return 0;

        usb_state = USB_STATE_SENT_REPLY;
        return len - bulk_in_residue;
    }
}

void usb_gadget_run(void)
{
    /* Wait for the host to configure us before handing over to fastboot. */
    usb_state = USB_STATE_GADGET_WAIT;
    while (!configured)
        usb_event_pump();

    fastboot_loop();
}
