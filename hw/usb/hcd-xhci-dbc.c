/*
 * USB xHCI controller emulation: Debug Capability (DbC)
 *
 * Copyright (c) 2026 Tom Sanders <tomsanders@live.nl>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "chardev/char-fe.h"
#include "system/dma.h"

#include "hcd-xhci.h"
#include "hcd-xhci-dbc.h"
#include "trace.h"

/* register offsets within the capability */
#define DBC_DCID            0x00
#define DBC_DCDB            0x04
#define DBC_DCERSTSZ        0x08
#define DBC_DCERSTBA_LO     0x10
#define DBC_DCERSTBA_HI     0x14
#define DBC_DCERDP_LO       0x18
#define DBC_DCERDP_HI       0x1c
#define DBC_DCCTRL          0x20
#define DBC_DCST            0x24
#define DBC_DCPORTSC        0x28
#define DBC_DCCP_LO         0x30
#define DBC_DCCP_HI         0x34
#define DBC_DCDDI1          0x38
#define DBC_DCDDI2          0x3c

/* DCID */
#define DCID_ERSTMAX_SHIFT      16

/* DCDB */
#define DCDB_TARGET_SHIFT       8
#define DCDB_TARGET_MASK        0xff

/* DCCTRL */
#define DCCTRL_DCR              (1u << 0)
#define DCCTRL_LSE              (1u << 1)  /* "port enable" in some drivers */
#define DCCTRL_HOT              (1u << 2)
#define DCCTRL_HIT              (1u << 3)
#define DCCTRL_DRC              (1u << 4)
#define DCCTRL_MAXBURST_SHIFT   16
#define DCCTRL_MAXBURST_MASK    0xff
#define DCCTRL_DEVADDR_SHIFT    24
#define DCCTRL_DEVADDR_MASK     0x7f
#define DCCTRL_DCE              (1u << 31)

/* DCST */
#define DCST_ER                 (1u << 0)
#define DCST_SBR                (1u << 1)
#define DCST_PORTNUM_SHIFT      24

/* DCPORTSC - the bit layout mirrors the operational PORTSC register */
#define DCPORTSC_CCS            (1u << 0)
#define DCPORTSC_PED            (1u << 1)
#define DCPORTSC_PR             (1u << 4)
#define DCPORTSC_PLS_SHIFT      5
#define DCPORTSC_PLS_MASK       0xf
#define DCPORTSC_SPEED_SHIFT    10
#define DCPORTSC_SPEED_MASK     0xf
#define DCPORTSC_CSC            (1u << 17)
#define DCPORTSC_PRC            (1u << 21)
#define DCPORTSC_PLC            (1u << 22)
#define DCPORTSC_CEC            (1u << 23)
#define DCPORTSC_RW1C           (DCPORTSC_CSC | DCPORTSC_PRC | \
                                 DCPORTSC_PLC | DCPORTSC_CEC)

/* endpoint types, as encoded in an endpoint context */
#define DBC_ET_BULK_OUT         2
#define DBC_ET_BULK_IN          6

#define DBC_PLS_U0              0
#define DBC_PLS_DISABLED        4
#define DBC_PLS_RX_DETECT       5
#define DBC_PLS_INACTIVE        6

/*
 * How long the debug host takes to enumerate us, and to finish a reset.
 * Not architectural - it exists so that DbC-Enabled and DbC-Resetting are
 * states software can actually observe rather than instants.
 */
#define XHCI_DBC_LINK_DELAY_NS  (1 * 1000 * 1000)
#define DBC_SPEED_SUPER         4

/*
 * DCDB doorbell targets (xHCI 1.2 table 7-18): 0 is the OUT transfer ring,
 * 1 the IN transfer ring, 2-255 are reserved.
 */
#define XHCI_DBC_DB_OUT         0
#define XHCI_DBC_DB_IN          1

/*
 * Endpoint IDs reported in DbC transfer events.  These are ordinary
 * xHCI DCIs, so EP 1 OUT is 2 and EP 1 IN is 3.  (Some Intel parts are
 * known to report 0 and 1 instead; drivers accept both.)
 */
#define XHCI_DBC_EPID_OUT       2
#define XHCI_DBC_EPID_IN        3

/* Debug Capability Context: info context, then the two endpoint contexts */
#define DBC_CTX_INFO_OFF        0x00
#define DBC_CTX_EP_OUT_OFF      0x40
#define DBC_CTX_EP_IN_OFF       0x80

/* the four string descriptors the DbCIC points at, in DbCIC order */
#define DBC_STRINGS             4
#define USB_DT_STRING           0x03

/*
 * A single event ring segment is supported, so DCERST Max is 0
 * (the maximum DCERSTSZ value is 2^0 == 1).
 */
#define XHCI_DBC_ERST_MAX       0

/* USB device address reported once the debug host has configured us */
#define XHCI_DBC_DEV_ADDR       1

/*
 * Root hub port the DbC reports itself attached to in DCST.  Reported as
 * 0 in the DbC-Off and DbC-Disconnected states, per 7.6.6.
 */
#define XHCI_DBC_PORT_NUM       1

#define XHCI_DBC_LINK_LIMIT     32
#define XHCI_DBC_TRANSFER_LIMIT 256

/* TRB transfer length is a 17 bit field */
#define DBC_TRB_LEN(t)          ((t).status & 0x1ffff)

static void xhci_dbc_kick_in(XHCIDbCState *dbc);

static inline dma_addr_t dbc_addr64(uint32_t low, uint32_t high)
{
    if (sizeof(dma_addr_t) == 4) {
        return low;
    }
    return low | (((dma_addr_t)high) << 32);
}

bool xhci_dbc_enabled(XHCIState *xhci)
{
    return xhci->dbc.enabled;
}

static bool xhci_dbc_running(XHCIDbCState *dbc)
{
    return (dbc->dcctrl & DCCTRL_DCE) && (dbc->dcctrl & DCCTRL_DCR) &&
           dbc->er_size != 0;
}

/* ------------------------------------------------------------------ */
/* event ring                                                          */
/* ------------------------------------------------------------------ */

static bool xhci_dbc_er_dp_idx(XHCIDbCState *dbc, unsigned int *idx)
{
    dma_addr_t erdp = dbc_addr64(dbc->dcerdp_low, dbc->dcerdp_high) &
                      ~(dma_addr_t)0xf;

    if (!dbc->er_size) {
        return false;
    }
    if (erdp < dbc->er_start ||
        erdp >= dbc->er_start + (dma_addr_t)TRB_SIZE * dbc->er_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: DCERDP out of bounds\n", __func__);
        return false;
    }
    *idx = (erdp - dbc->er_start) / TRB_SIZE;
    return true;
}

/* true if at least one more event can be queued without overrunning ERDP */
static bool xhci_dbc_er_space(XHCIDbCState *dbc)
{
    unsigned int dp_idx;

    if (!xhci_dbc_er_dp_idx(dbc, &dp_idx)) {
        return false;
    }
    return (dbc->er_ep_idx + 1) % dbc->er_size != dp_idx;
}

static bool xhci_dbc_er_not_empty(XHCIDbCState *dbc)
{
    unsigned int dp_idx;

    if (!xhci_dbc_er_dp_idx(dbc, &dp_idx)) {
        return false;
    }
    return dbc->er_ep_idx != dp_idx;
}

static void xhci_dbc_event(XHCIDbCState *dbc, XHCIEvent *event)
{
    XHCITRB ev_trb;
    dma_addr_t addr;

    if (!xhci_dbc_er_space(dbc)) {
        trace_usb_xhci_dbc_event_dropped(event->type, event->epid);
        return;
    }

    ev_trb.parameter = cpu_to_le64(event->ptr);
    ev_trb.status = cpu_to_le32(event->length | (event->ccode << 24));
    ev_trb.control = (event->slotid << 24) | (event->epid << 16) |
                     event->flags | (event->type << TRB_TYPE_SHIFT);
    if (dbc->er_pcs) {
        ev_trb.control |= TRB_C;
    }
    ev_trb.control = cpu_to_le32(ev_trb.control);

    trace_usb_xhci_dbc_queue_event(dbc->er_ep_idx, event->type, event->epid,
                                   event->ccode, event->ptr, event->length);

    /*
     * Software polls the cycle bit to find new events - the DbC has no
     * interrupt - so the rest of the TRB has to be visible before the
     * control dword that carries it.
     */
    addr = dbc->er_start + (dma_addr_t)TRB_SIZE * dbc->er_ep_idx;
    if (dma_memory_write(dbc->xhci->as, addr, &ev_trb, TRB_SIZE - 4,
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK ||
        dma_memory_write(dbc->xhci->as, addr + TRB_SIZE - 4, &ev_trb.control,
                         4, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DMA memory access failed!\n",
                      __func__);
        return;
    }

    dbc->er_ep_idx++;
    if (dbc->er_ep_idx >= dbc->er_size) {
        dbc->er_ep_idx = 0;
        dbc->er_pcs = !dbc->er_pcs;
    }
}

static void xhci_dbc_er_reset(XHCIDbCState *dbc)
{
    XHCIEvRingSeg seg;
    dma_addr_t erstba = dbc_addr64(dbc->dcerstba_low, dbc->dcerstba_high) &
                        ~(dma_addr_t)0xf;

    dbc->er_start = 0;
    dbc->er_size = 0;
    dbc->er_ep_idx = 0;
    dbc->er_pcs = true;

    if (dbc->dcerstsz == 0 || erstba == 0) {
        return;
    }
    if (dbc->dcerstsz != 1) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: only a single DbC event ring "
                      "segment is supported (DCERSTSZ=%u)\n",
                      __func__, dbc->dcerstsz);
        return;
    }
    if (dma_memory_read(dbc->xhci->as, erstba, &seg, sizeof(seg),
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DMA memory access failed!\n",
                      __func__);
        return;
    }
    le32_to_cpus(&seg.addr_low);
    le32_to_cpus(&seg.addr_high);
    le32_to_cpus(&seg.size);

    if (seg.size < 16 || seg.size > 4096) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: invalid DbC event ring segment size %u\n",
                      __func__, seg.size);
        return;
    }
    dbc->er_start = dbc_addr64(seg.addr_low, seg.addr_high);
    dbc->er_size = seg.size;

    trace_usb_xhci_dbc_er_reset(dbc->er_start, dbc->er_size);
}

/* ------------------------------------------------------------------ */
/* transfer rings                                                      */
/* ------------------------------------------------------------------ */

static bool xhci_dbc_ring_fetch(XHCIDbCState *dbc, XHCIDbCEp *ep, XHCITRB *trb)
{
    unsigned int link_cnt = 0;

    if (!ep->dequeue) {
        return false;
    }

    while (1) {
        if (dma_memory_read(dbc->xhci->as, ep->dequeue, trb, TRB_SIZE,
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: DMA memory access failed!\n",
                          __func__);
            return false;
        }
        trb->addr = ep->dequeue;
        trb->ccs = ep->ccs;
        le64_to_cpus(&trb->parameter);
        le32_to_cpus(&trb->status);
        le32_to_cpus(&trb->control);

        if ((trb->control & TRB_C) != ep->ccs) {
            /* ring is empty */
            return false;
        }

        if (TRB_TYPE(*trb) != TR_LINK) {
            ep->dequeue += TRB_SIZE;
            trace_usb_xhci_dbc_fetch_trb(trb->addr, TRB_TYPE(*trb),
                                         trb->parameter, trb->status);
            return true;
        }

        if (++link_cnt > XHCI_DBC_LINK_LIMIT) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: too many link TRBs in a row\n", __func__);
            return false;
        }
        ep->dequeue = trb->parameter & ~(uint64_t)0xf;
        if (trb->control & TRB_LK_TC) {
            ep->ccs = !ep->ccs;
        }
    }
}

static void xhci_dbc_xfer_event(XHCIDbCState *dbc, unsigned int epid,
                                dma_addr_t trb_addr, uint32_t residual,
                                TRBCCode ccode)
{
    XHCIEvent ev = {
        .type = ER_TRANSFER,
        .ccode = ccode,
        .ptr = trb_addr,
        .length = residual,
        .epid = epid,
    };

    xhci_dbc_event(dbc, &ev);
}

/*
 * Load one of the two bulk endpoint contexts out of the Debug Capability
 * Context and start its transfer ring.
 */
static void xhci_dbc_load_ep(XHCIDbCState *dbc, unsigned int idx,
                             dma_addr_t pctx)
{
    XHCIDbCEp *ep = &dbc->eps[idx];
    uint32_t ctx[5];
    uint64_t dequeue;
    unsigned int i, type;

    memset(ep, 0, sizeof(*ep));
    ep->pctx = pctx;

    if (dma_memory_read(dbc->xhci->as, pctx, ctx, sizeof(ctx),
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DMA memory access failed!\n",
                      __func__);
        ep->pctx = 0;
        return;
    }
    for (i = 0; i < ARRAY_SIZE(ctx); i++) {
        ctx[i] = le32_to_cpu(ctx[i]);
    }

    /*
     * The two rings are not interchangeable: the context at DbCC+0x40 is
     * the bulk OUT ring, which carries buffers to receive into, and the
     * one at DbCC+0x80 is the bulk IN ring, which carries data to send.
     * A context with the wrong endpoint type is a driver bug that would
     * be hard to see later, so refuse to run that endpoint rather than
     * quietly doing the right thing anyway.
     */
    type = (ctx[1] >> EP_TYPE_SHIFT) & EP_TYPE_MASK;
    if (type != (idx == XHCI_DBC_EP_OUT ? DBC_ET_BULK_OUT : DBC_ET_BULK_IN)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: DbC %s endpoint context has type %u, expected "
                      "%u; endpoint left halted\n", __func__,
                      idx == XHCI_DBC_EP_OUT ? "OUT" : "IN", type,
                      idx == XHCI_DBC_EP_OUT ? DBC_ET_BULK_OUT
                                             : DBC_ET_BULK_IN);
        ep->halted = true;
    }

    dequeue = ctx[2] | ((uint64_t)ctx[3] << 32);
    ep->ccs = dequeue & 1;
    ep->dequeue = dequeue & ~(uint64_t)0xf;

    /* the DbC maintains EP State in the guest visible context */
    ctx[0] = (ctx[0] & ~EP_STATE_MASK) | EP_RUNNING;
    ctx[0] = cpu_to_le32(ctx[0]);
    if (dma_memory_write(dbc->xhci->as, pctx, ctx, sizeof(ctx[0]),
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DMA memory access failed!\n",
                      __func__);
    }

    trace_usb_xhci_dbc_ep_start(idx, pctx, ep->dequeue, ep->ccs);
}

/* write the transfer ring position and the endpoint state back */
static void xhci_dbc_save_ep(XHCIDbCState *dbc, unsigned int idx,
                             uint32_t state)
{
    XHCIDbCEp *ep = &dbc->eps[idx];
    uint32_t ctx[4];
    uint64_t dequeue;

    if (!ep->pctx) {
        return;
    }
    if (dma_memory_read(dbc->xhci->as, ep->pctx, ctx, sizeof(ctx),
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return;
    }
    /*
     * A TRB that was fetched but not completed is given back to the guest
     * by rewinding the dequeue pointer past it.
     */
    dequeue = ep->trb_valid ? ep->trb_addr : ep->dequeue;
    dequeue |= ep->ccs ? 1 : 0;

    ctx[0] = cpu_to_le32((le32_to_cpu(ctx[0]) & ~EP_STATE_MASK) | state);
    ctx[2] = cpu_to_le32((uint32_t)dequeue);
    ctx[3] = cpu_to_le32((uint32_t)(dequeue >> 32));
    if (dma_memory_write(dbc->xhci->as, ep->pctx, ctx, sizeof(ctx),
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DMA memory access failed!\n",
                      __func__);
    }
}

/*
 * Halt one of the bulk endpoints (7.6.4.3).  The DbC does this when it
 * hits a TRB or buffer error, and software can do it by writing HOT or
 * HIT.  Either way the in-flight TRB is completed with Stall Error
 * whether or not it asked for an interrupt, the dequeue pointer and the
 * Halted state are written back, and the endpoint stays halted until the
 * debug host sends ClearFeature(ENDPOINT_HALT).
 */
static void xhci_dbc_halt_ep(XHCIDbCState *dbc, unsigned int idx,
                             uint64_t trb_addr, uint32_t residual)
{
    XHCIDbCEp *ep = &dbc->eps[idx];

    if (ep->halted) {
        return;
    }
    ep->halted = true;
    dbc->dcctrl |= idx == XHCI_DBC_EP_OUT ? DCCTRL_HOT : DCCTRL_HIT;
    trace_usb_xhci_dbc_halt(idx, trb_addr);

    if (trb_addr) {
        xhci_dbc_xfer_event(dbc, idx == XHCI_DBC_EP_OUT ? XHCI_DBC_EPID_OUT
                                                        : XHCI_DBC_EPID_IN,
                            trb_addr, residual, CC_STALL_ERROR);
    }
    ep->trb_valid = false;
    xhci_dbc_save_ep(dbc, idx, EP_HALTED);
}

/*
 * ClearFeature(ENDPOINT_HALT) from the debug host.  The endpoint goes to
 * Stopped, and picks up again from whatever TR Dequeue Pointer software
 * left in the endpoint context when the doorbell is next rung.
 */
static void xhci_dbc_unhalt_ep(XHCIDbCState *dbc, unsigned int idx)
{
    XHCIDbCEp *ep = &dbc->eps[idx];

    dbc->dcctrl &= ~(idx == XHCI_DBC_EP_OUT ? DCCTRL_HOT : DCCTRL_HIT);
    if (!ep->halted) {
        return;
    }
    ep->halted = false;
    ep->reload = true;
    xhci_dbc_save_ep(dbc, idx, EP_STOPPED);
}

/* pick the transfer ring back up from the endpoint context */
static void xhci_dbc_reload_ep(XHCIDbCState *dbc, unsigned int idx)
{
    XHCIDbCEp *ep = &dbc->eps[idx];
    uint32_t ctx[4];
    uint64_t dequeue;

    ep->reload = false;
    if (!ep->pctx ||
        dma_memory_read(dbc->xhci->as, ep->pctx, ctx, sizeof(ctx),
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return;
    }
    dequeue = le32_to_cpu(ctx[2]) | ((uint64_t)le32_to_cpu(ctx[3]) << 32);
    ep->ccs = dequeue & 1;
    ep->dequeue = dequeue & ~(uint64_t)0xf;
    ep->trb_valid = false;
    trace_usb_xhci_dbc_ep_start(idx, ep->pctx, ep->dequeue, ep->ccs);
}

/* ------------------------------------------------------------------ */
/* IN endpoint: guest -> debug host                                    */
/* ------------------------------------------------------------------ */

static void xhci_dbc_kick_in(XHCIDbCState *dbc)
{
    XHCIDbCEp *ep = &dbc->eps[XHCI_DBC_EP_IN];
    unsigned int count = 0;
    XHCITRB trb;

    if (!xhci_dbc_running(dbc) || ep->halted) {
        return;
    }
    if (ep->reload) {
        xhci_dbc_reload_ep(dbc, XHCI_DBC_EP_IN);
    }

    while (count++ < XHCI_DBC_TRANSFER_LIMIT) {
        g_autofree uint8_t *buf = NULL;
        uint32_t len;
        int done;

        /* keep room for the completion event, else retry on the next kick */
        if (!xhci_dbc_er_space(dbc)) {
            break;
        }
        if (!xhci_dbc_ring_fetch(dbc, ep, &trb)) {
            break;
        }
        if (TRB_TYPE(trb) != TR_NORMAL) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: unsupported TRB type %d on the DbC IN ring\n",
                          __func__, TRB_TYPE(trb));
            xhci_dbc_halt_ep(dbc, XHCI_DBC_EP_IN, trb.addr, 0);
            break;
        }

        len = DBC_TRB_LEN(trb);
        if (len == 0) {
            xhci_dbc_xfer_event(dbc, XHCI_DBC_EPID_IN, trb.addr, 0,
                                CC_SUCCESS);
            continue;
        }

        buf = g_malloc(len);
        if (trb.control & TRB_TR_IDT) {
            /* immediate data, at most 8 bytes held in the TRB itself */
            uint8_t immediate[8];

            if (len > sizeof(immediate)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "%s: immediate-data TRB with %u bytes\n",
                              __func__, len);
                xhci_dbc_halt_ep(dbc, XHCI_DBC_EP_IN, trb.addr, len);
                break;
            }
            stq_le_p(immediate, trb.parameter);
            memcpy(buf, immediate, len);
        } else if (dma_memory_read(dbc->xhci->as, trb.parameter, buf, len,
                                   MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: DMA memory access failed!\n",
                          __func__);
            xhci_dbc_halt_ep(dbc, XHCI_DBC_EP_IN, trb.addr, len);
            break;
        }

        done = qemu_chr_fe_write_all(&dbc->chr, buf, len);
        if (done < 0) {
            done = 0;
        }
        trace_usb_xhci_dbc_xfer_in(trb.addr, len, done);
        xhci_dbc_xfer_event(dbc, XHCI_DBC_EPID_IN, trb.addr, len - done,
                            (uint32_t)done < len ? CC_SHORT_PACKET
                                                 : CC_SUCCESS);
    }
}

/* ------------------------------------------------------------------ */
/* OUT endpoint: debug host -> guest                                   */
/* ------------------------------------------------------------------ */

/*
 * Make sure a data buffer is posted on the OUT ring, fetching and caching
 * the next usable TRB if needed.  Empty and malformed TRBs are completed
 * right away and skipped.
 */
static bool xhci_dbc_out_buffer(XHCIDbCState *dbc)
{
    XHCIDbCEp *ep = &dbc->eps[XHCI_DBC_EP_OUT];
    unsigned int count = 0;
    XHCITRB trb;

    if (ep->reload) {
        xhci_dbc_reload_ep(dbc, XHCI_DBC_EP_OUT);
    }

    while (!ep->trb_valid && count++ < XHCI_DBC_TRANSFER_LIMIT) {
        if (!xhci_dbc_er_space(dbc)) {
            return false;
        }
        if (!xhci_dbc_ring_fetch(dbc, ep, &trb)) {
            return false;
        }
        if (TRB_TYPE(trb) != TR_NORMAL) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: unsupported TRB type %d on the DbC OUT ring\n",
                          __func__, TRB_TYPE(trb));
            xhci_dbc_halt_ep(dbc, XHCI_DBC_EP_OUT, trb.addr, 0);
            return false;
        }
        if (DBC_TRB_LEN(trb) == 0) {
            xhci_dbc_xfer_event(dbc, XHCI_DBC_EPID_OUT, trb.addr, 0,
                                CC_SUCCESS);
            continue;
        }
        ep->trb_addr = trb.addr;
        ep->trb_buf = trb.parameter;
        ep->trb_len = DBC_TRB_LEN(trb);
        ep->trb_off = 0;
        ep->trb_valid = true;
    }

    return ep->trb_valid;
}

static int xhci_dbc_chr_can_receive(void *opaque)
{
    XHCIDbCState *dbc = opaque;
    XHCIDbCEp *ep = &dbc->eps[XHCI_DBC_EP_OUT];

    if (!xhci_dbc_running(dbc) || ep->halted) {
        return 0;
    }
    if (!xhci_dbc_out_buffer(dbc)) {
        return 0;
    }
    return ep->trb_len - ep->trb_off;
}

static void xhci_dbc_chr_receive(void *opaque, const uint8_t *buf, int size)
{
    XHCIDbCState *dbc = opaque;
    XHCIDbCEp *ep = &dbc->eps[XHCI_DBC_EP_OUT];

    while (size > 0) {
        uint32_t n;

        if (!xhci_dbc_running(dbc) || ep->halted) {
            break;
        }
        if (!xhci_dbc_out_buffer(dbc)) {
            break;
        }

        n = MIN((uint32_t)size, ep->trb_len - ep->trb_off);
        if (dma_memory_write(dbc->xhci->as, ep->trb_buf + ep->trb_off, buf, n,
                             MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: DMA memory access failed!\n",
                          __func__);
            xhci_dbc_halt_ep(dbc, XHCI_DBC_EP_OUT, ep->trb_addr,
                             ep->trb_len - ep->trb_off);
            break;
        }
        ep->trb_off += n;
        buf += n;
        size -= n;

        /*
         * Any amount of data received from the debug host terminates the
         * transfer, short of the posted buffer size if need be - that is
         * how a bulk OUT transfer behaves on the wire.
         */
        trace_usb_xhci_dbc_xfer_out(ep->trb_addr, ep->trb_len, ep->trb_off);
        ep->trb_valid = false;
        xhci_dbc_xfer_event(dbc, XHCI_DBC_EPID_OUT, ep->trb_addr,
                            ep->trb_len - ep->trb_off,
                            ep->trb_off < ep->trb_len ? CC_SHORT_PACKET
                                                      : CC_SUCCESS);
    }
}

/* ------------------------------------------------------------------ */
/* link state                                                          */
/* ------------------------------------------------------------------ */

/*
 * All four DCPORTSC change bits are ORed into an internal "port status
 * change event generation" variable; a Port Status Change Event is posted
 * only when that OR makes a 0 to 1 transition (7.6.4.2).  The Port ID
 * field of the event is always 0 on the DbC event ring.
 */
static void xhci_dbc_set_change(XHCIDbCState *dbc, uint32_t bits)
{
    bool armed = (dbc->dcportsc & DCPORTSC_RW1C) != 0;

    dbc->dcportsc |= bits;

    if (armed || !bits) {
        return;
    }
    if (dbc->er_size) {
        XHCIEvent ev = {
            .type = ER_PORT_STATUS_CHANGE,
            .ccode = CC_SUCCESS,
            .ptr = 0,
        };

        xhci_dbc_event(dbc, &ev);
    }
}

/*
 * Give an in-flight buffer back when the DbC leaves the Configured state
 * (7.6.4.4): complete it with USB Transaction Error whether or not IOC was
 * set, and leave the dequeue pointer past it.
 */
static void xhci_dbc_abort_transfers(XHCIDbCState *dbc)
{
    XHCIDbCEp *ep = &dbc->eps[XHCI_DBC_EP_OUT];

    if (!ep->trb_valid) {
        return;
    }
    ep->trb_valid = false;
    xhci_dbc_xfer_event(dbc, XHCI_DBC_EPID_OUT, ep->trb_addr,
                        ep->trb_len - ep->trb_off, CC_USB_TRANSACTION_ERROR);
}

/*
 * The DbC port state machine of 7.6.6.  Every state below is a pure
 * register state - the LTSSM underneath it is electrical and cannot be
 * modelled, but nothing in this table depends on that.  Per-state CCS,
 * PED, PR and DCR come from the register definitions in 7.6.8.4 and
 * 7.6.8.6 and the prose in 7.6.6.1-7.6.6.7; the PLS values are the
 * annotations Figure 7-8 carries.
 */
static const struct {
    const char *name;
    bool ccs, ped, pr, dcr;
    uint8_t pls;
} xhci_dbc_states[] = {
    [XHCI_DBC_OFF]          = { "off",          0, 0, 0, 0, DBC_PLS_DISABLED },
    [XHCI_DBC_DISCONNECTED] = { "disconnected", 0, 0, 0, 0, DBC_PLS_RX_DETECT },
    [XHCI_DBC_DISABLED]     = { "disabled",     1, 0, 0, 0, DBC_PLS_DISABLED },
    [XHCI_DBC_ENABLED]      = { "enabled",      1, 1, 0, 0, DBC_PLS_U0 },
    [XHCI_DBC_CONFIGURED]   = { "configured",   1, 1, 0, 1, DBC_PLS_U0 },
    [XHCI_DBC_RESETTING]    = { "resetting",    1, 0, 1, 0, DBC_PLS_RX_DETECT },
    [XHCI_DBC_ERROR]        = { "error",        1, 0, 0, 0, DBC_PLS_INACTIVE },
};

static void xhci_dbc_arm(XHCIDbCState *dbc, XHCIDbCPortState target)
{
    dbc->timer_target = target;
    if (dbc->timer) {
        timer_mod(dbc->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                              XHCI_DBC_LINK_DELAY_NS);
    }
}

static void xhci_dbc_enter(XHCIDbCState *dbc, XHCIDbCPortState next,
                           uint32_t change)
{
    XHCIDbCPortState prev = dbc->state;
    uint32_t portsc;

    if (prev == next && !change) {
        return;
    }
    dbc->state = next;
    trace_usb_xhci_dbc_state(xhci_dbc_states[prev].name,
                             xhci_dbc_states[next].name);

    /* project the state onto DCPORTSC */
    portsc = dbc->dcportsc & DCPORTSC_RW1C;
    if (xhci_dbc_states[next].ccs) {
        portsc |= DCPORTSC_CCS |
                  (DBC_SPEED_SUPER << DCPORTSC_SPEED_SHIFT);
    }
    if (xhci_dbc_states[next].ped) {
        portsc |= DCPORTSC_PED;
    }
    if (xhci_dbc_states[next].pr) {
        portsc |= DCPORTSC_PR;
    }
    portsc |= (uint32_t)xhci_dbc_states[next].pls << DCPORTSC_PLS_SHIFT;
    dbc->dcportsc = portsc;

    /* and onto DCCTRL */
    if (xhci_dbc_states[next].dcr) {
        dbc->dcctrl &= ~(DCCTRL_DEVADDR_MASK << DCCTRL_DEVADDR_SHIFT);
        dbc->dcctrl |= DCCTRL_DCR |
                       (XHCI_DBC_DEV_ADDR << DCCTRL_DEVADDR_SHIFT);
    } else if (dbc->dcctrl & DCCTRL_DCR) {
        /* leaving DbC-Configured: DRC is set and the doorbell goes dead */
        dbc->dcctrl &= ~(DCCTRL_DCR |
                         (DCCTRL_DEVADDR_MASK << DCCTRL_DEVADDR_SHIFT));
        dbc->dcctrl |= DCCTRL_DRC;
        xhci_dbc_abort_transfers(dbc);
    }

    /* the endpoints stay halted from a reset until reconfiguration */
    if (next == XHCI_DBC_RESETTING) {
        dbc->eps[XHCI_DBC_EP_OUT].halted = true;
        dbc->eps[XHCI_DBC_EP_IN].halted = true;
        dbc->dcctrl &= ~(DCCTRL_HOT | DCCTRL_HIT);
    } else if (next == XHCI_DBC_CONFIGURED) {
        dbc->eps[XHCI_DBC_EP_OUT].halted = false;
        dbc->eps[XHCI_DBC_EP_IN].halted = false;
    }

    xhci_dbc_set_change(dbc, change);

    /* the debug host enumerates us, then configures us */
    if (next == XHCI_DBC_ENABLED) {
        xhci_dbc_arm(dbc, XHCI_DBC_CONFIGURED);
    } else if (next == XHCI_DBC_RESETTING) {
        xhci_dbc_arm(dbc, XHCI_DBC_ENABLED);
    } else if (dbc->timer) {
        timer_del(dbc->timer);
    }

    /*
     * Nothing is fetched from the transfer rings here.  After the port is
     * configured the endpoints stay quiet "until software notifies the DbC
     * that the respective Transfer Rings have been initialized by ringing
     * their doorbells" (7.6.7.4) - reading a ring the driver has not
     * finished setting up would be reading garbage.
     */
}

static void xhci_dbc_timer(void *opaque)
{
    XHCIDbCState *dbc = opaque;
    uint32_t change = 0;

    if (dbc->timer_target == XHCI_DBC_ENABLED &&
        dbc->state == XHCI_DBC_RESETTING) {
        /* reset complete: PR falls, PED reasserts, PRC is set (7.6.7.4) */
        change = DCPORTSC_PRC;
    }
    xhci_dbc_enter(dbc, dbc->timer_target, change);
}

/*
 * Recompute the state from the things software and the chardev control:
 * DCE, whether a debug host is attached, and DCPORTSC.PED.
 */
static void xhci_dbc_update_link(XHCIDbCState *dbc)
{
    bool enabled = dbc->dcctrl & DCCTRL_DCE;
    bool connected = enabled && dbc->host_connected;

    if (!enabled) {
        xhci_dbc_enter(dbc, XHCI_DBC_OFF, 0);
        return;
    }
    if (!connected) {
        /* a disconnect from any state except Off lands here, and sets CSC */
        xhci_dbc_enter(dbc, XHCI_DBC_DISCONNECTED,
                       dbc->state > XHCI_DBC_DISCONNECTED ? DCPORTSC_CSC : 0);
        return;
    }
    if (dbc->state <= XHCI_DBC_DISCONNECTED) {
        /* a debug host appeared: the LMP exchange sets CSC (7.6.6.2) */
        dbc->port_enabled = true;
        xhci_dbc_enter(dbc, XHCI_DBC_ENABLED, DCPORTSC_CSC);
        return;
    }
    if (!dbc->port_enabled && dbc->state != XHCI_DBC_DISABLED &&
        dbc->state != XHCI_DBC_RESETTING) {
        xhci_dbc_enter(dbc, XHCI_DBC_DISABLED, 0);
    } else if (dbc->port_enabled && dbc->state == XHCI_DBC_DISABLED) {
        xhci_dbc_enter(dbc, XHCI_DBC_ENABLED, 0);
    }
}

/*
 * The debug host's control plane.  On real hardware these arrive as USB
 * link traffic - resets, SET_CONFIGURATION, ClearFeature, link errors -
 * which a chardev cannot carry in band, so they come in on a separate
 * chardev as one character each.  Without it the model still reaches
 * Off/Disconnected/Enabled/Disabled/Configured; this is what makes
 * Resetting and Error reachable too.
 */
static void xhci_dbc_ctrl_receive(void *opaque, const uint8_t *buf, int size)
{
    XHCIDbCState *dbc = opaque;
    int i;

    for (i = 0; i < size; i++) {
        trace_usb_xhci_dbc_ctrl(buf[i]);

        switch (buf[i]) {
        case 'r':                       /* Hot or Warm Reset from the host */
        case 'w':
            if (dbc->state >= XHCI_DBC_DISABLED) {
                xhci_dbc_enter(dbc, XHCI_DBC_RESETTING, 0);
            }
            break;
        case 'd':                       /* SET_CONFIGURATION(0) */
            if (dbc->state == XHCI_DBC_CONFIGURED) {
                xhci_dbc_enter(dbc, XHCI_DBC_ENABLED, 0);
            }
            break;
        case 'c':                       /* ClearFeature(ENDPOINT_HALT) */
            xhci_dbc_unhalt_ep(dbc, XHCI_DBC_EP_OUT);
            xhci_dbc_unhalt_ep(dbc, XHCI_DBC_EP_IN);
            break;
        case 'e':                       /* link error: Ux/Recovery -> Inactive */
            if (dbc->state >= XHCI_DBC_ENABLED) {
                xhci_dbc_enter(dbc, XHCI_DBC_ERROR, DCPORTSC_PLC);
            }
            break;
        case 'x':                       /* tPortConfiguration timeout */
            if (dbc->state >= XHCI_DBC_ENABLED) {
                xhci_dbc_enter(dbc, XHCI_DBC_DISABLED, DCPORTSC_CEC);
            }
            break;
        case 'k':                       /* the host retrains the link */
            if (dbc->state == XHCI_DBC_ERROR ||
                dbc->state == XHCI_DBC_DISABLED) {
                dbc->port_enabled = true;
                xhci_dbc_enter(dbc, XHCI_DBC_ENABLED, 0);
            }
            break;
        case '\r':
        case '\n':
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: unknown DbC control byte 0x%02x\n",
                          __func__, buf[i]);
            break;
        }
    }
}

static int xhci_dbc_ctrl_can_receive(void *opaque)
{
    return 8;
}

/*
 * Check over the Debug Capability Info Context (7.6.9.1).  The DbC would
 * hand these string descriptors to the debug host during enumeration; this
 * model does not enumerate, but a descriptor that the hardware would choke
 * on is a real driver bug and is cheap to catch here.  A length of zero
 * means "not defined", and the address is then ignored.
 */
static void xhci_dbc_check_dbcic(XHCIDbCState *dbc, dma_addr_t dccp)
{
    static const char * const names[DBC_STRINGS] = {
        "string 0 (languages)", "manufacturer", "product", "serial number"
    };
    uint64_t addr[DBC_STRINGS];
    uint32_t lengths;
    unsigned int i;

    if (dma_memory_read(dbc->xhci->as, dccp + DBC_CTX_INFO_OFF, addr,
                        sizeof(addr), MEMTXATTRS_UNSPECIFIED) != MEMTX_OK ||
        dma_memory_read(dbc->xhci->as, dccp + DBC_CTX_INFO_OFF + 0x20,
                        &lengths, sizeof(lengths),
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DMA memory access failed!\n",
                      __func__);
        return;
    }
    lengths = le32_to_cpu(lengths);

    for (i = 0; i < DBC_STRINGS; i++) {
        uint8_t want = (lengths >> (i * 8)) & 0xff;
        uint8_t hdr[2];

        addr[i] = le64_to_cpu(addr[i]) & ~(uint64_t)1;
        trace_usb_xhci_dbc_string(names[i], addr[i], want);

        if (!want) {
            continue;               /* not defined; the address is ignored */
        }
        if (!addr[i]) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: DbCIC %s has length %u but a null address\n",
                          __func__, names[i], want);
            continue;
        }
        if (dma_memory_read(dbc->xhci->as, addr[i], hdr, sizeof(hdr),
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: DbCIC %s descriptor is unreadable\n",
                          __func__, names[i]);
            continue;
        }
        if (hdr[1] != USB_DT_STRING) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: DbCIC %s descriptor has bDescriptorType "
                          "0x%02x, expected 0x%02x\n",
                          __func__, names[i], hdr[1], USB_DT_STRING);
        }
        if (hdr[0] != want) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: DbCIC %s says %u bytes but its bLength is "
                          "%u\n", __func__, names[i], want, hdr[0]);
        }
    }
    if (!((lengths >> 0) & 0xff)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: DbCIC declares no string 0; the debug host has "
                      "no supported-language list\n", __func__);
    }
}

static void xhci_dbc_start(XHCIDbCState *dbc)
{
    dma_addr_t dccp = dbc_addr64(dbc->dccp_low, dbc->dccp_high) &
                      ~(dma_addr_t)0xf;

    trace_usb_xhci_dbc_enable(dccp);

    xhci_dbc_er_reset(dbc);
    dbc->port_enabled = true;
    dbc->state = XHCI_DBC_DISCONNECTED;
    if (dccp) {
        xhci_dbc_check_dbcic(dbc, dccp);
        xhci_dbc_load_ep(dbc, XHCI_DBC_EP_OUT, dccp + DBC_CTX_EP_OUT_OFF);
        xhci_dbc_load_ep(dbc, XHCI_DBC_EP_IN, dccp + DBC_CTX_EP_IN_OFF);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: DbC enabled with a null context pointer\n",
                      __func__);
    }
    xhci_dbc_update_link(dbc);
}

static void xhci_dbc_stop(XHCIDbCState *dbc)
{
    unsigned int i;

    trace_usb_xhci_dbc_disable();

    /* leave DbC-Configured while the event ring is still valid */
    xhci_dbc_enter(dbc, XHCI_DBC_OFF, 0);
    for (i = 0; i < ARRAY_SIZE(dbc->eps); i++) {
        xhci_dbc_save_ep(dbc, i, EP_STOPPED);
        memset(&dbc->eps[i], 0, sizeof(dbc->eps[i]));
    }
    /* disabling the DbC clears the port status change bits (7.6.4.2) */
    dbc->dcportsc &= ~DCPORTSC_RW1C;
    dbc->er_start = 0;
    dbc->er_size = 0;
    dbc->er_ep_idx = 0;
    dbc->er_pcs = true;
}

/* ------------------------------------------------------------------ */
/* register access                                                     */
/* ------------------------------------------------------------------ */

static uint64_t xhci_dbc_read(void *ptr, hwaddr reg, unsigned size)
{
    XHCIDbCState *dbc = ptr;
    uint32_t ret;

    switch (reg) {
    case DBC_DCID:
        /* the DbC is the last entry of the extended capability list */
        ret = XHCI_DBC_CAP_ID | (XHCI_DBC_ERST_MAX << DCID_ERSTMAX_SHIFT);
        break;
    case DBC_DCDB:
        ret = 0;
        break;
    case DBC_DCERSTSZ:
        ret = dbc->dcerstsz;
        break;
    case DBC_DCERSTBA_LO:
        ret = dbc->dcerstba_low;
        break;
    case DBC_DCERSTBA_HI:
        ret = dbc->dcerstba_high;
        break;
    case DBC_DCERDP_LO:
        ret = dbc->dcerdp_low;
        break;
    case DBC_DCERDP_HI:
        ret = dbc->dcerdp_high;
        break;
    case DBC_DCCTRL:
        ret = dbc->dcctrl;
        break;
    case DBC_DCST:
        ret = 0;
        if (xhci_dbc_er_not_empty(dbc)) {
            ret |= DCST_ER;
        }
        if (dbc->sbr) {
            ret |= DCST_SBR;
        }
        /* the port is only assigned to the DbC once a host is connected */
        if (dbc->dcportsc & DCPORTSC_CCS) {
            ret |= (uint32_t)XHCI_DBC_PORT_NUM << DCST_PORTNUM_SHIFT;
        }
        break;
    case DBC_DCPORTSC:
        ret = dbc->dcportsc;
        break;
    case DBC_DCCP_LO:
        ret = dbc->dccp_low;
        break;
    case DBC_DCCP_HI:
        ret = dbc->dccp_high;
        break;
    case DBC_DCDDI1:
        ret = dbc->dcddi1;
        break;
    case DBC_DCDDI2:
        ret = dbc->dcddi2;
        break;
    default:
        ret = 0;
        break;
    }

    trace_usb_xhci_dbc_read(reg, ret);
    return ret;
}

static void xhci_dbc_write(void *ptr, hwaddr reg, uint64_t val, unsigned size)
{
    XHCIDbCState *dbc = ptr;
    uint32_t old;

    trace_usb_xhci_dbc_write(reg, val);

    switch (reg) {
    case DBC_DCDB:
        /* the doorbell is disabled while DRC is set (7.6.8.4) */
        if (dbc->dcctrl & DCCTRL_DRC) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: DCDB rung while DRC is set; ignored\n",
                          __func__);
            break;
        }
        switch ((val >> DCDB_TARGET_SHIFT) & DCDB_TARGET_MASK) {
        case XHCI_DBC_DB_OUT:
            /* buffers were added to the OUT ring: resume the chardev */
            qemu_chr_fe_accept_input(&dbc->chr);
            break;
        case XHCI_DBC_DB_IN:
            xhci_dbc_kick_in(dbc);
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: reserved DCDB target 0x%" PRIx64 "\n",
                          __func__, val);
            break;
        }
        break;

    case DBC_DCERSTSZ:
        dbc->dcerstsz = val & 0xffff;
        break;
    case DBC_DCERSTBA_LO:
        /* bits 3:0 are RsvdP (7.6.8.3.2) */
        dbc->dcerstba_low = val & 0xfffffff0;
        break;
    case DBC_DCERSTBA_HI:
        dbc->dcerstba_high = val;
        break;
    case DBC_DCERDP_LO:
        /* bits 2:0 are the Dequeue ERST Segment Index, and are RW */
        dbc->dcerdp_low = val & 0xfffffff7;
        /* events may have been blocked by a full ring */
        xhci_dbc_kick_in(dbc);
        qemu_chr_fe_accept_input(&dbc->chr);
        break;
    case DBC_DCERDP_HI:
        dbc->dcerdp_high = val;
        break;

    case DBC_DCCTRL:
        old = dbc->dcctrl;
        /* DRC is write-1-to-clear */
        dbc->dcctrl &= ~(val & DCCTRL_DRC);
        /*
         * HOT and HIT are RW1S: software sets them to halt a pipe, and
         * only the debug host's ClearFeature(ENDPOINT_HALT) clears them.
         * Outside Run Mode they read 0 and writes have no effect
         * (7.6.8.4).
         */
        if (dbc->dcctrl & DCCTRL_DCR) {
            if (val & DCCTRL_HOT) {
                xhci_dbc_halt_ep(dbc, XHCI_DBC_EP_OUT, 0, 0);
            }
            if (val & DCCTRL_HIT) {
                xhci_dbc_halt_ep(dbc, XHCI_DBC_EP_IN, 0, 0);
            }
        }
        /* LSE, max burst request and DCE are writable */
        dbc->dcctrl &= ~(DCCTRL_LSE | DCCTRL_DCE);
        dbc->dcctrl |= val & (DCCTRL_LSE | DCCTRL_DCE);
        if ((old ^ dbc->dcctrl) & DCCTRL_DCE) {
            if (dbc->dcctrl & DCCTRL_DCE) {
                xhci_dbc_start(dbc);
            } else {
                xhci_dbc_stop(dbc);
            }
        }
        break;

    case DBC_DCPORTSC:
        dbc->dcportsc &= ~(val & DCPORTSC_RW1C);
        /*
         * PED is writable: clearing it puts the port in DbC-Disabled, so
         * the debug host sees a disconnected device (7.6.8.6).
         */
        if (dbc->dcctrl & DCCTRL_DCE) {
            dbc->port_enabled = !!(val & DCPORTSC_PED);
            xhci_dbc_update_link(dbc);
        }
        break;

    case DBC_DCCP_LO:
        dbc->dccp_low = val & 0xfffffff0;
        break;
    case DBC_DCCP_HI:
        dbc->dccp_high = val;
        break;
    case DBC_DCDDI1:
        dbc->dcddi1 = val;
        break;
    case DBC_DCDDI2:
        dbc->dcddi2 = val;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to read-only or reserved DbC register 0x%"
                      HWADDR_PRIx "\n", __func__, reg);
        break;
    }
}

const MemoryRegionOps xhci_dbc_ops = {
    .read = xhci_dbc_read,
    .write = xhci_dbc_write,
    /* the register block is only ever accessed 32 bits at a time */
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ------------------------------------------------------------------ */
/* device model plumbing                                               */
/* ------------------------------------------------------------------ */

static void xhci_dbc_chr_event(void *opaque, QEMUChrEvent event)
{
    XHCIDbCState *dbc = opaque;

    switch (event) {
    case CHR_EVENT_OPENED:
        dbc->host_connected = true;
        xhci_dbc_update_link(dbc);
        break;
    case CHR_EVENT_CLOSED:
        dbc->host_connected = false;
        xhci_dbc_update_link(dbc);
        break;
    default:
        break;
    }
}

/*
 * A host controller reset drives the DbC port to DbC-Off (7.6.6.1) unless
 * the implementation reports DCST.SBR, in which case only a chip or system
 * bus reset does (7.6.8.5).  Which one applies is a property of the part,
 * so it is a property of the model too.
 */
void xhci_dbc_hc_reset(XHCIState *xhci)
{
    if (xhci->dbc.sbr) {
        return;
    }
    xhci_dbc_reset(xhci);
}

void xhci_dbc_reset(XHCIState *xhci)
{
    XHCIDbCState *dbc = &xhci->dbc;

    if (!dbc->enabled) {
        return;
    }

    dbc->dcctrl = 0;
    dbc->dcportsc = DBC_PLS_RX_DETECT << DCPORTSC_PLS_SHIFT;
    dbc->dcerstsz = 0;
    dbc->dcerstba_low = 0;
    dbc->dcerstba_high = 0;
    dbc->dcerdp_low = 0;
    dbc->dcerdp_high = 0;
    dbc->dccp_low = 0;
    dbc->dccp_high = 0;
    dbc->dcddi1 = 0;
    dbc->dcddi2 = 0;

    dbc->er_start = 0;
    dbc->er_size = 0;
    dbc->er_ep_idx = 0;
    dbc->er_pcs = true;
    dbc->port_enabled = true;
    dbc->state = XHCI_DBC_OFF;
    if (dbc->timer) {
        timer_del(dbc->timer);
    }

    memset(dbc->eps, 0, sizeof(dbc->eps));
}

void xhci_dbc_realize(XHCIState *xhci)
{
    XHCIDbCState *dbc = &xhci->dbc;

    dbc->xhci = xhci;
    dbc->enabled = dbc->prop_enabled ||
                   qemu_chr_fe_backend_connected(&dbc->chr);
    if (!dbc->enabled) {
        return;
    }

    dbc->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, xhci_dbc_timer, dbc);
    xhci_dbc_reset(xhci);
    qemu_chr_fe_set_handlers(&dbc->chr, xhci_dbc_chr_can_receive,
                             xhci_dbc_chr_receive, xhci_dbc_chr_event,
                             NULL, dbc, NULL, true);
    if (qemu_chr_fe_backend_connected(&dbc->ctrl)) {
        qemu_chr_fe_set_handlers(&dbc->ctrl, xhci_dbc_ctrl_can_receive,
                                 xhci_dbc_ctrl_receive, NULL,
                                 NULL, dbc, NULL, true);
    }
}

void xhci_dbc_unrealize(XHCIState *xhci)
{
    XHCIDbCState *dbc = &xhci->dbc;

    if (!dbc->enabled) {
        return;
    }
    qemu_chr_fe_set_handlers(&dbc->chr, NULL, NULL, NULL, NULL, NULL, NULL,
                             false);
    qemu_chr_fe_set_handlers(&dbc->ctrl, NULL, NULL, NULL, NULL, NULL, NULL,
                             false);
    if (dbc->timer) {
        timer_free(dbc->timer);
        dbc->timer = NULL;
    }
}

static bool xhci_dbc_needed(void *opaque)
{
    XHCIState *xhci = opaque;

    return xhci->dbc.enabled;
}

static int xhci_dbc_post_load(void *opaque, int version_id)
{
    XHCIState *xhci = opaque;
    XHCIDbCState *dbc = &xhci->dbc;

    dbc->xhci = xhci;
    /*
     * Re-derive the event ring cache rather than migrating it: the guest
     * visible ERST is the authority.  The ring position itself is
     * migrated, so only the segment location has to be recovered.
     */
    if (dbc->dcctrl & DCCTRL_DCE) {
        uint32_t ep_idx = dbc->er_ep_idx;
        bool pcs = dbc->er_pcs;

        xhci_dbc_er_reset(dbc);
        dbc->er_ep_idx = ep_idx;
        dbc->er_pcs = pcs;
    }
    return 0;
}

static const VMStateDescription vmstate_xhci_dbc_ep = {
    .name = "xhci-dbc-ep",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(dequeue,   XHCIDbCEp),
        VMSTATE_BOOL(ccs,         XHCIDbCEp),
        VMSTATE_UINT64(pctx,      XHCIDbCEp),
        VMSTATE_BOOL(halted,      XHCIDbCEp),
        VMSTATE_BOOL(trb_valid,   XHCIDbCEp),
        VMSTATE_UINT64(trb_addr,  XHCIDbCEp),
        VMSTATE_UINT64(trb_buf,   XHCIDbCEp),
        VMSTATE_UINT32(trb_len,   XHCIDbCEp),
        VMSTATE_UINT32(trb_off,   XHCIDbCEp),
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_xhci_dbc = {
    .name = "xhci-core/dbc",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = xhci_dbc_needed,
    .post_load = xhci_dbc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(dbc.dcctrl,       XHCIState),
        VMSTATE_UINT32(dbc.dcportsc,     XHCIState),
        VMSTATE_UINT32(dbc.dcerstsz,     XHCIState),
        VMSTATE_UINT32(dbc.dcerstba_low, XHCIState),
        VMSTATE_UINT32(dbc.dcerstba_high, XHCIState),
        VMSTATE_UINT32(dbc.dcerdp_low,   XHCIState),
        VMSTATE_UINT32(dbc.dcerdp_high,  XHCIState),
        VMSTATE_UINT32(dbc.dccp_low,     XHCIState),
        VMSTATE_UINT32(dbc.dccp_high,    XHCIState),
        VMSTATE_UINT32(dbc.dcddi1,       XHCIState),
        VMSTATE_UINT32(dbc.dcddi2,       XHCIState),
        VMSTATE_UINT32(dbc.er_ep_idx,    XHCIState),
        VMSTATE_BOOL(dbc.er_pcs,         XHCIState),
        VMSTATE_BOOL(dbc.host_connected, XHCIState),
        VMSTATE_BOOL(dbc.port_enabled,   XHCIState),
        VMSTATE_UINT32(dbc.state,        XHCIState),
        VMSTATE_UINT32(dbc.timer_target, XHCIState),
        VMSTATE_TIMER_PTR(dbc.timer,     XHCIState),
        VMSTATE_STRUCT_ARRAY(dbc.eps, XHCIState, 2, 1,
                             vmstate_xhci_dbc_ep, XHCIDbCEp),
        VMSTATE_END_OF_LIST()
    }
};
