/*
 * USB xHCI controller emulation: Debug Capability (DbC)
 *
 * Copyright (c) 2026 Tom Sanders <tomsanders@live.nl>
 *
 * Models the xHCI Debug Capability (extended capability ID 0x0a) as
 * described by the xHCI specification, chapter 7.6.  The two bulk
 * endpoints of the debug device are bridged to a QEMU chardev, which
 * plays the role of the machine at the far end of the debug cable.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef HW_USB_HCD_XHCI_DBC_H
#define HW_USB_HCD_XHCI_DBC_H

#include "hcd-xhci.h"

/* xHCI extended capability ID and size of the register block */
#define XHCI_DBC_CAP_ID     0x0a
#define XHCI_DBC_LEN        0x40

/*
 * Endpoint indices into XHCIDbCState.eps.  Note that these are *not* the
 * endpoint IDs reported in transfer events, see XHCI_DBC_EPID_*.
 */
#define XHCI_DBC_EP_OUT     0
#define XHCI_DBC_EP_IN      1

bool xhci_dbc_enabled(XHCIState *xhci);
void xhci_dbc_realize(XHCIState *xhci);
void xhci_dbc_unrealize(XHCIState *xhci);
void xhci_dbc_reset(XHCIState *xhci);
void xhci_dbc_hc_reset(XHCIState *xhci);

extern const MemoryRegionOps xhci_dbc_ops;
extern const VMStateDescription vmstate_xhci_dbc;

#endif
