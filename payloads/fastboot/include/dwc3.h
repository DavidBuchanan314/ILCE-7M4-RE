#ifndef FASTBOOT_DWC3_H
#define FASTBOOT_DWC3_H

#include "io.h"

enum dwc3_status {
    DWC3_OK                 =  0,
    DWC3_ERR_NOT_DWC3       = -1,   /* GSNPSID did not read 0x5533xxxx      */
    DWC3_ERR_SOFTRESET      = -2,   /* DCTL.CSFTRST never cleared           */
    DWC3_ERR_NOT_STARTED    = -3,   /* DSTS.DEVCTRLHLT never cleared        */
};

/*
 * Core bring-up through to "device mode, high speed, ready to be told to
 * connect". Assumes usb_phy_init() has already ungated clocks and released
 * resets. Does not assert the D+ pullup; call dwc3_connect() for that.
 */
int dwc3_core_init(void);

/*
 * Assert the D+ pullup (DCTL.RUN_STOP) and wait for DSTS.DEVCTRLHLT to clear.
 * The write landing is not the same as the device being on the bus.
 */
int  dwc3_connect(void);
void dwc3_disconnect(void);

/* Whether the TCA block acknowledged during init; not fatal. */
int  dwc3_tca_acked(void);

/* Latched during dwc3_core_init(). */
u32 dwc3_revision(void);

/* Non-zero if this is a DWC_usb31 core (as the CXD90057 is), which changes
 * where the revision lives and mandates a post-soft-reset delay. */
int dwc3_is_usb31(void);


/* ---- endpoint layer ----------------------------------------------------- */

/*
 * Issue a DEPCMD on a physical endpoint and wait for CMDACT to clear.
 * Returns the 4-bit command status (0 = success) or a negative value on
 * timeout.
 */
int dwc3_depcmd(u32 phys_ep, u32 cmd, u32 p0, u32 p1, u32 p2);

/* Called repeatedly while dwc3_depcmd() waits. */
void dwc3_wait_tick(void);

/*
 * DEPSTARTCFG on physical ep0, parameter 0. Resets the transfer-resource
 * assignment of every endpoint, so it belongs once per bus reset and not at
 * SET_CONFIGURATION time.
 */
int dwc3_ep_start_config(void);

/*
 * SETEPCONFIG plus SETTRANSFRESOURCE for one endpoint, addressed by its
 * PHYSICAL number -- dwc3 folds the direction bit in, so USB endpoint 0x81 is
 * physical 3, and that is also what goes in DEPCFG's EP_NUMBER.
 */
int dwc3_ep_config(u32 phys_ep, u32 type, u32 maxpacket);

/* Enable/disable in DALEPENA. */
void dwc3_ep_enable(u32 phys_ep);
void dwc3_ep_disable(u32 phys_ep);

/* STARTTRANSFER with a prepared TRB. Returns the transfer resource index in
 * *rsc_idx (needed later for ENDTRANSFER), or negative on failure. */
int dwc3_ep_start_xfer(u32 phys_ep, u64 trb_addr, u32 *rsc_idx);

/* ENDTRANSFER, using the resource index STARTTRANSFER handed back. */
int dwc3_ep_end_xfer(u32 phys_ep, u32 rsc_idx);

int dwc3_ep_set_stall(u32 phys_ep);
int dwc3_ep_clear_stall(u32 phys_ep);

/* Pop one event; 0 is not a valid event, so it means the buffer is empty. */
u32 dwc3_event_poll(void);

void dwc3_set_address(u32 addr);

#endif /* FASTBOOT_DWC3_H */
