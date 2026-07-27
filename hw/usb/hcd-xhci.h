/*
 * USB xHCI controller emulation
 *
 * Copyright (c) 2011 Securiforest
 * Date: 2011-05-11 ;  Author: Hector Martin <hector@marcansoft.com>
 * Based on usb-ohci.c, emulates Renesas NEC USB 3.0
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_USB_HCD_XHCI_H
#define HW_USB_HCD_XHCI_H
#include "qom/object.h"

#include "hw/usb.h"
#include "hw/usb/xhci.h"
#include "system/dma.h"
#include "chardev/char-fe.h"

OBJECT_DECLARE_SIMPLE_TYPE(XHCIState, XHCI)

/* Very pessimistic, let's hope it's enough for all cases */
#define EV_QUEUE (((3 * 24) + 16) * XHCI_MAXSLOTS)

typedef struct XHCIStreamContext XHCIStreamContext;
typedef struct XHCIEPContext XHCIEPContext;

enum xhci_flags {
    XHCI_FLAG_ENABLE_STREAMS = 1,
};

typedef enum TRBType {
    TRB_RESERVED = 0,
    TR_NORMAL,
    TR_SETUP,
    TR_DATA,
    TR_STATUS,
    TR_ISOCH,
    TR_LINK,
    TR_EVDATA,
    TR_NOOP,
    CR_ENABLE_SLOT,
    CR_DISABLE_SLOT,
    CR_ADDRESS_DEVICE,
    CR_CONFIGURE_ENDPOINT,
    CR_EVALUATE_CONTEXT,
    CR_RESET_ENDPOINT,
    CR_STOP_ENDPOINT,
    CR_SET_TR_DEQUEUE,
    CR_RESET_DEVICE,
    CR_FORCE_EVENT,
    CR_NEGOTIATE_BW,
    CR_SET_LATENCY_TOLERANCE,
    CR_GET_PORT_BANDWIDTH,
    CR_FORCE_HEADER,
    CR_NOOP,
    ER_TRANSFER = 32,
    ER_COMMAND_COMPLETE,
    ER_PORT_STATUS_CHANGE,
    ER_BANDWIDTH_REQUEST,
    ER_DOORBELL,
    ER_HOST_CONTROLLER,
    ER_DEVICE_NOTIFICATION,
    ER_MFINDEX_WRAP,
    /* vendor specific bits */
    CR_VENDOR_NEC_FIRMWARE_REVISION  = 49,
    CR_VENDOR_NEC_CHALLENGE_RESPONSE = 50,
} TRBType;

typedef enum TRBCCode {
    CC_INVALID = 0,
    CC_SUCCESS,
    CC_DATA_BUFFER_ERROR,
    CC_BABBLE_DETECTED,
    CC_USB_TRANSACTION_ERROR,
    CC_TRB_ERROR,
    CC_STALL_ERROR,
    CC_RESOURCE_ERROR,
    CC_BANDWIDTH_ERROR,
    CC_NO_SLOTS_ERROR,
    CC_INVALID_STREAM_TYPE_ERROR,
    CC_SLOT_NOT_ENABLED_ERROR,
    CC_EP_NOT_ENABLED_ERROR,
    CC_SHORT_PACKET,
    CC_RING_UNDERRUN,
    CC_RING_OVERRUN,
    CC_VF_ER_FULL,
    CC_PARAMETER_ERROR,
    CC_BANDWIDTH_OVERRUN,
    CC_CONTEXT_STATE_ERROR,
    CC_NO_PING_RESPONSE_ERROR,
    CC_EVENT_RING_FULL_ERROR,
    CC_INCOMPATIBLE_DEVICE_ERROR,
    CC_MISSED_SERVICE_ERROR,
    CC_COMMAND_RING_STOPPED,
    CC_COMMAND_ABORTED,
    CC_STOPPED,
    CC_STOPPED_LENGTH_INVALID,
    CC_MAX_EXIT_LATENCY_TOO_LARGE_ERROR = 29,
    CC_ISOCH_BUFFER_OVERRUN = 31,
    CC_EVENT_LOST_ERROR,
    CC_UNDEFINED_ERROR,
    CC_INVALID_STREAM_ID_ERROR,
    CC_SECONDARY_BANDWIDTH_ERROR,
    CC_SPLIT_TRANSACTION_ERROR
} TRBCCode;

#define TRB_SIZE 16

typedef struct XHCITRB {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
    dma_addr_t addr;
    bool ccs;
} XHCITRB;

typedef struct XHCIEvRingSeg {
    uint32_t addr_low;
    uint32_t addr_high;
    uint32_t size;
    uint32_t rsvd;
} XHCIEvRingSeg;

#define TRB_C               (1<<0)
#define TRB_TYPE_SHIFT          10
#define TRB_TYPE_MASK       0x3f
#define TRB_TYPE(t)         (((t).control >> TRB_TYPE_SHIFT) & TRB_TYPE_MASK)

#define TRB_EV_ED           (1<<2)

#define TRB_TR_ENT          (1<<1)
#define TRB_TR_ISP          (1<<2)
#define TRB_TR_NS           (1<<3)
#define TRB_TR_CH           (1<<4)
#define TRB_TR_IOC          (1<<5)
#define TRB_TR_IDT          (1<<6)
#define TRB_TR_TBC_SHIFT        7
#define TRB_TR_TBC_MASK     0x3
#define TRB_TR_BEI          (1<<9)
#define TRB_TR_TLBPC_SHIFT      16
#define TRB_TR_TLBPC_MASK   0xf
#define TRB_TR_FRAMEID_SHIFT    20
#define TRB_TR_FRAMEID_MASK 0x7ff
#define TRB_TR_SIA          (1<<31)

#define TRB_TR_DIR          (1<<16)

#define TRB_CR_SLOTID_SHIFT     24
#define TRB_CR_SLOTID_MASK  0xff
#define TRB_CR_EPID_SHIFT       16
#define TRB_CR_EPID_MASK    0x1f

#define TRB_CR_BSR          (1<<9)
#define TRB_CR_DC           (1<<9)

#define TRB_LK_TC           (1<<1)

#define TRB_INTR_SHIFT          22
#define TRB_INTR_MASK       0x3ff
#define TRB_INTR(t)         (((t).status >> TRB_INTR_SHIFT) & TRB_INTR_MASK)

#define EP_TYPE_MASK        0x7
#define EP_TYPE_SHIFT           3

#define EP_STATE_MASK       0x7
#define EP_DISABLED         (0<<0)
#define EP_RUNNING          (1<<0)
#define EP_HALTED           (2<<0)
#define EP_STOPPED          (3<<0)
#define EP_ERROR            (4<<0)

typedef struct XHCIRing {
    dma_addr_t dequeue;
    bool ccs;
} XHCIRing;

typedef struct XHCIPort {
    XHCIState *xhci;
    uint32_t portsc;
    uint32_t portnr;
    USBPort  *uport;
    uint32_t speedmask;
    char name[20];
    MemoryRegion mem;
} XHCIPort;

typedef struct XHCISlot {
    bool enabled;
    bool addressed;
    uint16_t intr;
    dma_addr_t ctx;
    USBPort *uport;
    XHCIEPContext *eps[31];
} XHCISlot;

typedef struct XHCIEvent {
    TRBType type;
    TRBCCode ccode;
    uint64_t ptr;
    uint32_t length;
    uint32_t flags;
    uint8_t slotid;
    uint8_t epid;
} XHCIEvent;

typedef struct XHCIInterrupter {
    uint32_t iman;
    uint32_t imod;
    uint32_t erstsz;
    uint32_t erstba_low;
    uint32_t erstba_high;
    uint32_t erdp_low;
    uint32_t erdp_high;

    bool msix_used, er_pcs;

    dma_addr_t er_start;
    uint32_t er_size;
    unsigned int er_ep_idx;

    /* kept for live migration compat only */
    bool er_full_unused;
    XHCIEvent ev_buffer[EV_QUEUE];
    unsigned int ev_buffer_put;
    unsigned int ev_buffer_get;

} XHCIInterrupter;

/*
 * DbC port states (xHCI 1.2 section 7.6.6).  The register projection of
 * each state is in xhci_dbc_enter(); only the LTSSM beneath them is
 * electrical and therefore out of reach of a model.
 */
typedef enum XHCIDbCPortState {
    XHCI_DBC_OFF,
    XHCI_DBC_DISCONNECTED,
    XHCI_DBC_DISABLED,
    XHCI_DBC_ENABLED,
    XHCI_DBC_CONFIGURED,
    XHCI_DBC_RESETTING,
    XHCI_DBC_ERROR,
} XHCIDbCPortState;

/* one of the two bulk endpoints of the Debug Capability */
typedef struct XHCIDbCEp {
    /* transfer ring */
    uint64_t dequeue;
    bool     ccs;
    uint64_t pctx;          /* endpoint context in guest memory */
    bool     halted;
    bool     reload;        /* pick the dequeue pointer back up on kick */

    /* data buffer fetched from the ring but not yet completed */
    bool     trb_valid;
    uint64_t trb_addr;
    uint64_t trb_buf;
    uint32_t trb_len;
    uint32_t trb_off;
} XHCIDbCEp;

typedef struct XHCIDbCState {
    XHCIState  *xhci;
    CharFrontend chr;
    CharFrontend ctrl;       /* debug host control plane, see 7.6.6 */

    /* properties */
    bool        prop_enabled;
    bool        sbr;             /* DCST.SBR: HCRST does not reset the DbC */

    bool        enabled;         /* capability present in the ext cap list */
    bool        host_connected;  /* the far end of the debug cable is there */
    bool        port_enabled;    /* software's DCPORTSC.PED intent */

    XHCIDbCPortState state;
    XHCIDbCPortState timer_target;
    QEMUTimer  *timer;           /* enumeration and reset completion */

    /* registers */
    uint32_t    dcctrl;
    uint32_t    dcportsc;
    uint32_t    dcerstsz;
    uint32_t    dcerstba_low;
    uint32_t    dcerstba_high;
    uint32_t    dcerdp_low;
    uint32_t    dcerdp_high;
    uint32_t    dccp_low;
    uint32_t    dccp_high;
    uint32_t    dcddi1;
    uint32_t    dcddi2;

    /* cached event ring segment */
    dma_addr_t  er_start;
    uint32_t    er_size;
    uint32_t    er_ep_idx;
    bool        er_pcs;

    XHCIDbCEp   eps[2];
} XHCIDbCState;

typedef struct XHCIState {
    DeviceState parent;

    USBBus bus;
    MemoryRegion mem;
    MemoryRegion *dma_mr;
    AddressSpace *as;
    MemoryRegion mem_cap;
    MemoryRegion mem_oper;
    MemoryRegion mem_runtime;
    MemoryRegion mem_doorbell;
    MemoryRegion mem_dbc;

    /* offset of the operational registers, i.e. the value of CAPLENGTH */
    uint32_t off_oper;

    /* properties */
    uint32_t numports_2;
    uint32_t numports_3;
    uint32_t numintrs;
    uint32_t numslots;
    uint32_t flags;
    uint32_t max_pstreams_mask;
    void (*intr_update)(XHCIState *s, int n, bool enable);
    bool (*intr_raise)(XHCIState *s, int n, bool level);
    /*
     * Callback for special-casing interrupter mapping support. NULL for most
     * implementations, for defaulting to enabled mapping unless numintrs == 1.
     */
    bool (*intr_mapping_supported)(XHCIState *s);
    DeviceState *hostOpaque;

    /* Operational Registers */
    uint32_t usbcmd;
    uint32_t usbsts;
    uint32_t dnctrl;
    uint32_t crcr_low;
    uint32_t crcr_high;
    uint32_t dcbaap_low;
    uint32_t dcbaap_high;
    uint32_t config;

    USBPort  uports[MAX_CONST(XHCI_MAXPORTS_2, XHCI_MAXPORTS_3)];
    XHCIPort ports[XHCI_MAXPORTS];
    XHCISlot slots[XHCI_MAXSLOTS];
    uint32_t numports;

    /* Runtime Registers */
    int64_t mfindex_start;
    QEMUTimer *mfwrap_timer;
    XHCIInterrupter intr[XHCI_MAXINTRS];

    XHCIRing cmd_ring;

    /* Debug Capability */
    XHCIDbCState dbc;

    bool nec_quirks;
} XHCIState;

extern const VMStateDescription vmstate_xhci;
bool xhci_get_flag(XHCIState *xhci, enum xhci_flags bit);
void xhci_set_flag(XHCIState *xhci, enum xhci_flags bit);
#endif
