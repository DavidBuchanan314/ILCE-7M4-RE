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
 * listening.
 */
int usb_gadget_init(void);   /* 0 on success, else DEPCMD status */

/* Waits for SET_CONFIGURATION, then hands over to fastboot_loop(). */
void usb_gadget_run(void);

/*
 * Drain pending events. Called from every wait loop below, so ep0 keeps being
 * serviced while a bulk transfer is outstanding.
 */
void usb_event_pump(void);

/* Blocking bulk transfers. Return bytes actually transferred. */
u32 usb_bulk_recv(void *buf, u32 len);
u32 usb_bulk_send(const void *buf, u32 len);

/* True once the host has issued SET_CONFIGURATION. */
int usb_is_configured(void);

/* Non-zero if the bulk path failed to come up or wedged. */
#define USB_BULK_ERR_STARTCFG       9
#define USB_BULK_ERR_CFG_OUT        10
#define USB_BULK_ERR_CFG_IN         11
#define USB_BULK_ERR_OUT_CMD        12
#define USB_BULK_ERR_OUT_TIMEOUT    13
#define USB_BULK_ERR_IN_CMD         14
#define USB_BULK_ERR_IN_TIMEOUT     15

extern int usb_bulk_error;

/* Cancel queued bulk transfers. */
void usb_bulk_abort(void);

/* Clear a recoverable bulk error so the command loop can carry on. Returns 0
 * if it recovered, negative if the error is not one it can. */
int usb_bulk_recover(void);

void usb_bulk_enable_pending(void);

#endif /* FASTBOOT_USB_H */
