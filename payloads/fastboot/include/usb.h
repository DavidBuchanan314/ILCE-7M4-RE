#ifndef FASTBOOT_USB_H
#define FASTBOOT_USB_H

#include "io.h"

/* ---- USB 2.0 spec constants -------------------------------------------- */
#define USB_DIR_OUT                 0x00
#define USB_DIR_IN                  0x80

#define USB_TYPE_STANDARD           0x00
#define USB_TYPE_CLASS              0x20
#define USB_TYPE_VENDOR             0x40
#define USB_TYPE_MASK               0x60

#define USB_RECIP_DEVICE            0x00
#define USB_RECIP_INTERFACE         0x01
#define USB_RECIP_ENDPOINT          0x02
#define USB_RECIP_MASK              0x1f

#define USB_REQ_GET_STATUS          0x00
#define USB_REQ_CLEAR_FEATURE       0x01
#define USB_REQ_SET_FEATURE         0x03
#define USB_REQ_SET_ADDRESS         0x05
#define USB_REQ_GET_DESCRIPTOR      0x06
#define USB_REQ_SET_DESCRIPTOR      0x07
#define USB_REQ_GET_CONFIGURATION   0x08
#define USB_REQ_SET_CONFIGURATION   0x09
#define USB_REQ_GET_INTERFACE       0x0a
#define USB_REQ_SET_INTERFACE       0x0b

#define USB_DT_DEVICE               0x01
#define USB_DT_CONFIG               0x02
#define USB_DT_STRING               0x03
#define USB_DT_INTERFACE            0x04
#define USB_DT_ENDPOINT             0x05
#define USB_DT_DEVICE_QUALIFIER     0x06
#define USB_DT_OTHER_SPEED_CONFIG   0x07
#define USB_DT_BOS                  0x0f

#define USB_ENDPOINT_XFER_BULK      0x02

/* Setup packet, as it arrives on the wire (little-endian). */
struct usb_ctrlrequest {
    u8  bRequestType;
    u8  bRequest;
    u16 wValue;
    u16 wIndex;
    u16 wLength;
} __attribute__((packed));

#define EP0_MAXPACKET       64
#define BULK_MAXPACKET      512     /* high speed */

/* Bulk endpoint addresses as the host sees them. */
#define EP_BULK_OUT_ADDR    0x01
#define EP_BULK_IN_ADDR     0x81

/*
 * Configure ep0 and arm a SETUP. Must be called BEFORE the D+ pullup goes up,
 * or the host's first SETUP after bus reset arrives at an endpoint that is not
 * listening. (A reset event would re-arm it, so this is belt-and-braces --
 * but the first enumeration attempt is the one most likely to be watched.)
 */
int usb_gadget_init(void);   /* 0 on success, else DEPCMD status */

/*
 * Run the enumeration/event loop. Never returns: once the device is configured
 * it keeps servicing control traffic and hands bulk data to the fastboot layer.
 */
void usb_gadget_run(void);

/*
 * Process at most one pending event. Every blocking operation below calls this
 * in its wait loop, so control traffic keeps being serviced while a bulk
 * transfer is outstanding -- the host can issue GET_STATUS or re-read a
 * descriptor at any time, and a device that stops answering ep0 mid-transfer
 * gets dropped.
 */
void usb_event_pump(void);

/* Blocking bulk transfers. Return bytes actually transferred. */
u32 usb_bulk_recv(void *buf, u32 len);
u32 usb_bulk_send(const void *buf, u32 len);

/* True once the host has issued SET_CONFIGURATION. */
int usb_is_configured(void);

/*
 * Non-zero if the bulk path failed to come up or wedged. A stuck bulk endpoint
 * looks exactly like a hang from the host side, so this is reported over the
 * LED -- the one channel that still works when USB does not.
 */
#define USB_BULK_ERR_STARTCFG       9
#define USB_BULK_ERR_CFG_OUT        10
#define USB_BULK_ERR_CFG_IN         11
#define USB_BULK_ERR_OUT_CMD        12
#define USB_BULK_ERR_OUT_TIMEOUT    13
#define USB_BULK_ERR_IN_CMD         14
#define USB_BULK_ERR_IN_TIMEOUT     15

extern int usb_bulk_error;

/*
 * Coarse progress marker, blinked continuously by the LED heartbeat so it is
 * readable even when USB itself is wedged.
 */
/*
 * ep0 tracing. Small numbers so they are easy to count; these override the
 * coarse states below while a control transfer is in progress. The steady
 * state on an idle, healthy device is EP0_ARMED -- a SETUP is outstanding and
 * we are waiting for the host.
 */
#define EP0_TRACE_ARMED             1   /* SETUP TRB queued, awaiting host    */
#define EP0_TRACE_GOT_SETUP         2   /* SETUP received and handled         */
#define EP0_TRACE_DATA_QUEUED       3   /* data stage queued                  */
#define EP0_TRACE_STATUS_QUEUED     4   /* status stage queued                */
#define EP0_TRACE_ARM_FAILED        5   /* STARTTRANSFER for SETUP failed     */

#define USB_STATE_UNCONFIGURED      1
#define USB_STATE_ENABLING_BULK     2
#define USB_STATE_CONFIGURED        3
#define USB_STATE_AWAIT_CMD         4
#define USB_STATE_GOT_CMD           5
#define USB_STATE_SENT_REPLY        6
/*
 * "ISSUING" states are set before a command that can hang, so a wedge shows up
 * as a steady count rather than as silence. "FAILED" states are outcomes.
 */
#define USB_STATE_CFG_OUT_ISSUING   7
#define USB_STATE_CFG_IN_ISSUING    8
#define USB_STATE_CFG_OUT_FAILED    12
#define USB_STATE_CFG_IN_FAILED     13
#define USB_STATE_WAIT_RECONFIG     11  /* reset seen; awaiting SET_CONFIGURATION */
#define USB_STATE_IN_RESET          14  /* inside on_reset()                      */
#define USB_STATE_RESET_EP0_FAIL    15  /* ep0_init() failed after a bus reset    */
/*
 * Every remaining place the code can sit gets its own number. A state that
 * persists must correspond to a loop, so an untagged loop is an invisible one
 * -- which is exactly how USB_STATE_CONFIGURED ended up being reported from
 * somewhere it was never set.
 */
#define USB_STATE_GADGET_WAIT       16  /* usb_gadget_run(), first configuration */
#define USB_STATE_FB_LOOP_TOP       17  /* top of a fastboot_loop() iteration     */
#define USB_STATE_IN_SET_CONFIG     18  /* inside the SET_CONFIGURATION handler   */
#define USB_STATE_BULK_WAIT_OUT     19  /* OUT queued, waiting for completion     */
#define USB_STATE_BULK_WAIT_IN      20  /* IN queued, waiting for completion      */

extern volatile u32 usb_state;
void usb_bulk_enable_pending(void);

#endif /* FASTBOOT_USB_H */
