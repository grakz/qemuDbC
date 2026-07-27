/*
 * QTest testcase for the xHCI Debug Capability (DbC)
 *
 * Copyright (c) 2026 Tom Sanders <tomsanders@live.nl>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The test drives the DbC the way a guest debug driver would: it walks the
 * extended capability list, builds the event ring, the debug capability
 * context and the two bulk transfer rings in guest memory, and then round
 * trips data between those rings and the chardev the DbC is bridged to.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/pci.h"
#include "libqos/pci-pc.h"
#include "qemu/sockets.h"

/* xHCI capability registers */
#define XHCI_HCCPARAMS1     0x10

/* DbC register offsets, relative to the start of the capability */
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

#define DCCTRL_DCR          (1u << 0)
#define DCCTRL_LSE          (1u << 1)
#define DCCTRL_HOT          (1u << 2)
#define DCCTRL_HIT          (1u << 3)
#define DCCTRL_DRC          (1u << 4)
#define DCCTRL_DCE          (1u << 31)

#define DCST_ER             (1u << 0)
#define DCST_SBR            (1u << 1)

#define DCPORTSC_CCS        (1u << 0)
#define DCPORTSC_PED        (1u << 1)
#define DCPORTSC_PR         (1u << 4)
#define DCPORTSC_CSC        (1u << 17)
#define DCPORTSC_PRC        (1u << 21)
#define DCPORTSC_RW1C_ALL   ((1u << 17) | (1u << 21) | (1u << 22) | (1u << 23))

#define XHCI_EXT_CAP_DBC    0x0a

/* TRB encoding */
#define TRB_C               (1u << 0)
#define TRB_TYPE_SHIFT      10
#define TR_NORMAL           1
#define TRB_TR_IDT          (1u << 6)
#define TR_LINK             6
#define TRB_LK_TC           (1u << 1)
#define ER_TRANSFER         32
#define ER_PORT_STATUS      34
#define CC_SUCCESS          1
#define CC_STALL_ERROR      6
#define CC_SHORT_PACKET     13

/*
 * Two different numbering spaces: the DCDB doorbell targets the OUT ring
 * with 0 and the IN ring with 1, while transfer events report the ordinary
 * xHCI DCI of the endpoint, 2 for EP 1 OUT and 3 for EP 1 IN.
 */
#define DB_OUT              0
#define DB_IN               1
#define EPID_OUT            2
#define EPID_IN             3

/* guest memory layout, in a quiet corner of RAM (nothing else runs here) */
#define MEM_BASE            0x01000000
#define ADDR_ERST           (MEM_BASE + 0x0000)
#define ADDR_EVRING         (MEM_BASE + 0x1000)
#define ADDR_CTX            (MEM_BASE + 0x2000)
#define ADDR_RING_OUT       (MEM_BASE + 0x3000)
#define ADDR_RING_IN        (MEM_BASE + 0x4000)
#define ADDR_BUF_OUT        (MEM_BASE + 0x5000)
#define ADDR_BUF_IN         (MEM_BASE + 0x6000)
#define ADDR_STRINGS        (MEM_BASE + 0x7000)
#define MEM_SIZE            0x8000

#define EVRING_SIZE         16

typedef struct DbCTest {
    QTestState *qts;
    QPCIBus    *bus;
    QPCIDevice *dev;
    QPCIBar     bar;
    uint32_t    cap;        /* offset of the DbC register block in the BAR */
    int         sock;       /* host side of the "debug cable" */
    int         ctrl;       /* the debug host's control plane */
    uint32_t    er_idx;     /* next event ring slot we expect to be filled */
    bool        er_ccs;     /* consumer cycle state, flips on each lap */
    uint32_t    in_enq;     /* IN transfer ring enqueue slot */
    bool        in_cycle;   /* IN transfer ring producer cycle state */
} DbCTest;

/* the IN transfer ring is four TRBs, the last of which is the link TRB */
#define IN_RING_TRBS        4

static uint32_t dbc_readl(DbCTest *t, uint32_t reg)
{
    return qpci_io_readl(t->dev, t->bar, t->cap + reg);
}

static void dbc_writel(DbCTest *t, uint32_t reg, uint32_t val)
{
    qpci_io_writel(t->dev, t->bar, t->cap + reg, val);
}

static void write_trb(DbCTest *t, uint64_t addr, uint64_t parameter,
                      uint32_t status, uint32_t control)
{
    qtest_writeq(t->qts, addr, parameter);
    qtest_writel(t->qts, addr + 8, status);
    qtest_writel(t->qts, addr + 12, control);
}

/*
 * Read the control dword first: it carries the cycle bit, and the DbC
 * publishes it only once the rest of the TRB is in place.
 */
static void read_trb(DbCTest *t, uint64_t addr, uint64_t *parameter,
                     uint32_t *status, uint32_t *control)
{
    *control = qtest_readl(t->qts, addr + 12);
    *parameter = qtest_readq(t->qts, addr);
    *status = qtest_readl(t->qts, addr + 8);
}

/*
 * Wait for the DbC to post the next event and return its TRB.  The cycle
 * bit of a fresh event matches the consumer cycle state, which flips every
 * time the ring wraps - checking for a set cycle bit instead would happily
 * re-consume the previous lap's events.
 */
static bool wait_event(DbCTest *t, uint64_t *parameter, uint32_t *status,
                       uint32_t *control)
{
    uint64_t addr = ADDR_EVRING + 16 * t->er_idx;
    int i;

    for (i = 0; i < 1000; i++) {
        read_trb(t, addr, parameter, status, control);
        if (!!(*control & TRB_C) == t->er_ccs) {
            t->er_idx++;
            if (t->er_idx >= EVRING_SIZE) {
                t->er_idx = 0;
                t->er_ccs = !t->er_ccs;
            }
            /* tell the DbC we consumed up to and including this TRB */
            dbc_writel(t, DBC_DCERDP_HI, 0);
            dbc_writel(t, DBC_DCERDP_LO, ADDR_EVRING + 16 * t->er_idx);
            return true;
        }
        g_usleep(1000);
    }
    return false;
}

static uint32_t event_type(uint32_t control)
{
    return (control >> TRB_TYPE_SHIFT) & 0x3f;
}

static uint32_t event_epid(uint32_t control)
{
    return (control >> 16) & 0x1f;
}

static uint32_t event_cc(uint32_t status)
{
    return status >> 24;
}

/*
 * Post one TRB on the IN transfer ring, laying down the link TRB and
 * flipping the producer cycle state when the ring wraps.  Returns the
 * address of the TRB, which is what the transfer event will point at.
 */
static uint64_t dbc_post_in(DbCTest *t, uint64_t parameter, uint32_t len,
                            uint32_t flags)
{
    uint64_t addr;

    if (t->in_enq == IN_RING_TRBS - 1) {
        write_trb(t, ADDR_RING_IN + t->in_enq * 16, ADDR_RING_IN, 0,
                  (TR_LINK << TRB_TYPE_SHIFT) | TRB_LK_TC |
                  (t->in_cycle ? TRB_C : 0));
        t->in_enq = 0;
        t->in_cycle = !t->in_cycle;
    }

    addr = ADDR_RING_IN + t->in_enq * 16;
    write_trb(t, addr, parameter, len,
              (TR_NORMAL << TRB_TYPE_SHIFT) | flags |
              (t->in_cycle ? TRB_C : 0));
    t->in_enq++;
    return addr;
}

/* find the DbC by walking the xHCI extended capability list */
static uint32_t find_dbc_cap(DbCTest *t)
{
    uint32_t hccparams = qpci_io_readl(t->dev, t->bar, XHCI_HCCPARAMS1);
    uint32_t off = (hccparams >> 16) << 2;
    int limit = 64;

    while (off && limit--) {
        uint32_t val = qpci_io_readl(t->dev, t->bar, off);
        uint32_t id = val & 0xff;
        uint32_t next = (val >> 8) & 0xff;

        if (id == XHCI_EXT_CAP_DBC) {
            return off;
        }
        if (!next) {
            break;
        }
        off += next << 2;
    }
    return 0;
}

static void dbc_setup(DbCTest *t)
{
    g_autofree uint8_t *zero = g_malloc0(MEM_SIZE);
    uint32_t dcid, dcctrl, dcportsc;
    uint64_t parameter;
    uint32_t status, control;
    int i;

    qtest_memwrite(t->qts, MEM_BASE, zero, MEM_SIZE);

    t->cap = find_dbc_cap(t);
    g_assert_cmpuint(t->cap, !=, 0);

    dcid = dbc_readl(t, DBC_DCID);
    g_assert_cmpuint(dcid & 0xff, ==, XHCI_EXT_CAP_DBC);
    /* the DbC is the last entry of the list */
    g_assert_cmpuint((dcid >> 8) & 0xff, ==, 0);

    /* the capability is quiescent until software enables it */
    g_assert_cmpuint(dbc_readl(t, DBC_DCCTRL) & DCCTRL_DCR, ==, 0);
    g_assert_cmpuint(dbc_readl(t, DBC_DCPORTSC) & DCPORTSC_CCS, ==, 0);

    /* event ring segment table with a single segment */
    qtest_writel(t->qts, ADDR_ERST + 0, ADDR_EVRING);
    qtest_writel(t->qts, ADDR_ERST + 4, 0);
    qtest_writel(t->qts, ADDR_ERST + 8, EVRING_SIZE);
    qtest_writel(t->qts, ADDR_ERST + 12, 0);

    dbc_writel(t, DBC_DCERSTSZ, 1);
    dbc_writel(t, DBC_DCERSTBA_HI, 0);
    dbc_writel(t, DBC_DCERSTBA_LO, ADDR_ERST);
    dbc_writel(t, DBC_DCERDP_HI, 0);
    dbc_writel(t, DBC_DCERDP_LO, ADDR_EVRING);
    t->er_ccs = true;
    t->in_enq = 0;
    t->in_cycle = true;

    /*
     * Debug capability context: the info context is followed by the bulk
     * OUT and bulk IN endpoint contexts.  Endpoint type 2 is bulk OUT,
     * 6 is bulk IN; CErr is 3 and the max packet size 1024.
     */
    qtest_writel(t->qts, ADDR_CTX + 0x40 + 4,
                 (3 << 1) | (2 << 3) | (1024 << 16));
    qtest_writeq(t->qts, ADDR_CTX + 0x40 + 8, ADDR_RING_OUT | 1);
    qtest_writel(t->qts, ADDR_CTX + 0x80 + 4,
                 (3 << 1) | (6 << 3) | (1024 << 16));
    qtest_writeq(t->qts, ADDR_CTX + 0x80 + 8, ADDR_RING_IN | 1);

    /*
     * The DbC info context: a language-ID string and a product string,
     * with the other two declared absent by a zero length.  The model
     * validates these on enable.
     */
    qtest_memwrite(t->qts, ADDR_STRINGS, "\x04\x03\x09\x04", 4);
    qtest_memwrite(t->qts, ADDR_STRINGS + 8, "\x08\x03T\0o\0s\0", 8);
    qtest_writeq(t->qts, ADDR_CTX + 0x00, ADDR_STRINGS);
    qtest_writeq(t->qts, ADDR_CTX + 0x10, ADDR_STRINGS + 8);
    qtest_writel(t->qts, ADDR_CTX + 0x20, (8 << 16) | 4);

    dbc_writel(t, DBC_DCCP_HI, 0);
    dbc_writel(t, DBC_DCCP_LO, ADDR_CTX);
    dbc_writel(t, DBC_DCDDI1, (0x1d6b << 16) | 0x00);
    dbc_writel(t, DBC_DCDDI2, (0x0010 << 16) | 0x0011);

    dbc_writel(t, DBC_DCCTRL, DCCTRL_DCE | DCCTRL_LSE);

    /*
     * The chardev peer is already attached, so the port goes straight to
     * DbC-Enabled: connected and enabled, but not yet configured by the
     * debug host.  A driver that treats CCS as "ready" trips here.
     */
    dcportsc = dbc_readl(t, DBC_DCPORTSC);
    g_assert_cmpuint(dcportsc & DCPORTSC_CCS, ==, DCPORTSC_CCS);
    g_assert_cmpuint(dcportsc & DCPORTSC_PED, ==, DCPORTSC_PED);
    g_assert_cmpuint(dbc_readl(t, DBC_DCCTRL) & DCCTRL_DCR, ==, 0);

    /* enumeration completes on its own; then we are DbC-Configured */
    qtest_clock_step_next(t->qts);
    for (i = 0; i < 1000; i++) {
        dcctrl = dbc_readl(t, DBC_DCCTRL);
        dcportsc = dbc_readl(t, DBC_DCPORTSC);
        if ((dcctrl & DCCTRL_DCR) && (dcportsc & DCPORTSC_CCS)) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_cmpuint(dcctrl & DCCTRL_DCR, ==, DCCTRL_DCR);
    /* DRC is set when DCR *clears*, so entering Configured leaves it clear */
    g_assert_cmpuint(dcctrl & DCCTRL_DRC, ==, 0);
    g_assert_cmpuint(dcportsc & DCPORTSC_CCS, ==, DCPORTSC_CCS);
    g_assert_cmpuint(dcportsc & DCPORTSC_PED, ==, DCPORTSC_PED);
    g_assert_cmpuint(dcportsc & DCPORTSC_CSC, ==, DCPORTSC_CSC);
    /* the root hub port is only reported once a debug host is attached */
    g_assert_cmpuint(dbc_readl(t, DBC_DCST) >> 24, !=, 0);

    /* bringing the link up posts a port status change event */
    g_assert_true(wait_event(t, &parameter, &status, &control));
    g_assert_cmpuint(event_type(control), ==, ER_PORT_STATUS);
    /* the Port ID field is always 0 on the DbC event ring */
    g_assert_cmpuint(parameter, ==, 0);

    /* the endpoint contexts are now marked running */
    g_assert_cmpuint(qtest_readl(t->qts, ADDR_CTX + 0x40) & 0x7, ==, 1);
    g_assert_cmpuint(qtest_readl(t->qts, ADDR_CTX + 0x80) & 0x7, ==, 1);

    /* change bits are write-1-to-clear; PED must survive the write */
    dbc_writel(t, DBC_DCPORTSC, DCPORTSC_CSC | DCPORTSC_PED);
    g_assert_cmpuint(dbc_readl(t, DBC_DCPORTSC) & DCPORTSC_CSC, ==, 0);
    g_assert_cmpuint(dbc_readl(t, DBC_DCPORTSC) & DCPORTSC_PED, ==,
                     DCPORTSC_PED);
}

/* the doorbell is disabled while DRC is set, and PED is writable */
static void dbc_test_port_disable(DbCTest *t)
{
    uint64_t parameter;
    uint32_t status, control;
    int i;

    /* clearing PED leaves DbC-Configured: DCR drops and DRC is set */
    dbc_writel(t, DBC_DCPORTSC, 0);
    g_assert_cmpuint(dbc_readl(t, DBC_DCCTRL) & DCCTRL_DCR, ==, 0);
    g_assert_cmpuint(dbc_readl(t, DBC_DCCTRL) & DCCTRL_DRC, ==, DCCTRL_DRC);
    g_assert_cmpuint(dbc_readl(t, DBC_DCPORTSC) & DCPORTSC_PED, ==, 0);
    /* CCS stays set: the cable is still plugged in */
    g_assert_cmpuint(dbc_readl(t, DBC_DCPORTSC) & DCPORTSC_CCS, ==,
                     DCPORTSC_CCS);
    /*
     * No event: DCPORTSC has no port-enable-change bit, and only the four
     * change bits generate Port Status Change Events.  Software that
     * disabled the port learns of the exit from DCCTRL.DRC.
     */
    g_assert_false(wait_event(t, &parameter, &status, &control));

    /* the doorbell is dead while DRC is set */
    dbc_post_in(t, ADDR_BUF_IN, 4, 0);
    dbc_writel(t, DBC_DCDB, DB_IN << 8);
    g_assert_false(wait_event(t, &parameter, &status, &control));

    /*
     * Re-enable.  That lands in DbC-Enabled, not straight back in
     * DbC-Configured - the debug host has to configure us again first.
     */
    dbc_writel(t, DBC_DCPORTSC, DCPORTSC_PED | DCPORTSC_RW1C_ALL);
    g_assert_cmpuint(dbc_readl(t, DBC_DCCTRL) & DCCTRL_DCR, ==, 0);
    qtest_clock_step_next(t->qts);
    g_assert_cmpuint(dbc_readl(t, DBC_DCCTRL) & DCCTRL_DCR, ==, DCCTRL_DCR);

    /* then clear DRC to re-arm the doorbell */
    dbc_writel(t, DBC_DCCTRL, dbc_readl(t, DBC_DCCTRL) | DCCTRL_DRC);
    g_assert_cmpuint(dbc_readl(t, DBC_DCCTRL) & DCCTRL_DRC, ==, 0);
    /* with DRC cleared the doorbell works again and the TRB is picked up */
    dbc_writel(t, DBC_DCDB, DB_IN << 8);
    g_assert_true(wait_event(t, &parameter, &status, &control));
    g_assert_cmpuint(event_type(control), ==, ER_TRANSFER);
    g_assert_cmpuint(event_epid(control), ==, EPID_IN);
    for (i = 0; i < 4 && wait_event(t, &parameter, &status, &control); i++) {
        /* drain anything else the disable/enable cycle posted */
    }
}

/* debug host -> guest, over the DbC bulk OUT endpoint */
static void dbc_test_out(DbCTest *t)
{
    static const char msg[] = "hello from the debug host";
    g_autofree uint8_t *got = g_malloc0(sizeof(msg));
    uint64_t parameter;
    uint32_t status, control;
    ssize_t done;

    /* post a receive buffer and ring the OUT doorbell */
    write_trb(t, ADDR_RING_OUT, ADDR_BUF_OUT, 64,
              (TR_NORMAL << TRB_TYPE_SHIFT) | TRB_C);
    dbc_writel(t, DBC_DCDB, DB_OUT << 8);

    done = send(t->sock, msg, sizeof(msg), 0);
    g_assert_cmpint(done, ==, sizeof(msg));

    g_assert_true(wait_event(t, &parameter, &status, &control));
    g_assert_cmpuint(event_type(control), ==, ER_TRANSFER);
    g_assert_cmpuint(event_epid(control), ==, EPID_OUT);
    g_assert_cmpuint(event_cc(status), ==, CC_SHORT_PACKET);
    g_assert_cmpuint(parameter, ==, ADDR_RING_OUT);
    /* residual: the part of the 64 byte buffer that stayed empty */
    g_assert_cmpuint(status & 0xffffff, ==, 64 - sizeof(msg));

    qtest_memread(t->qts, ADDR_BUF_OUT, got, sizeof(msg));
    g_assert_cmpstr((char *)got, ==, msg);
}

/* guest -> debug host, over the DbC bulk IN endpoint */
static void dbc_test_in(DbCTest *t)
{
    static const char msg[] = "hello from the debug target";
    g_autofree uint8_t *got = g_malloc0(sizeof(msg));
    uint64_t parameter, trb;
    uint32_t status, control;
    size_t total = 0;
    int i;

    qtest_memwrite(t->qts, ADDR_BUF_IN, msg, sizeof(msg));
    trb = dbc_post_in(t, ADDR_BUF_IN, sizeof(msg), 0);
    dbc_writel(t, DBC_DCDB, DB_IN << 8);

    g_assert_true(wait_event(t, &parameter, &status, &control));
    g_assert_cmpuint(event_type(control), ==, ER_TRANSFER);
    g_assert_cmpuint(event_epid(control), ==, EPID_IN);
    g_assert_cmpuint(event_cc(status), ==, CC_SUCCESS);
    g_assert_cmpuint(parameter, ==, trb);
    g_assert_cmpuint(status & 0xffffff, ==, 0);

    for (i = 0; i < 1000 && total < sizeof(msg); i++) {
        ssize_t done = recv(t->sock, got + total, sizeof(msg) - total,
                            MSG_DONTWAIT);
        if (done > 0) {
            total += done;
            continue;
        }
        g_usleep(1000);
    }
    g_assert_cmpuint(total, ==, sizeof(msg));
    g_assert_cmpstr((char *)got, ==, msg);
}

/* guest -> debug host with the payload carried inside the TRB itself */
static void dbc_test_in_immediate(DbCTest *t)
{
    static const char msg[5] = "imm!";
    uint64_t payload = 0;
    uint64_t parameter, trb;
    uint32_t status, control;
    char got[sizeof(msg)];
    size_t total = 0;
    int i;

    for (i = sizeof(msg) - 1; i >= 0; i--) {
        payload = (payload << 8) | (uint8_t)msg[i];
    }
    trb = dbc_post_in(t, payload, sizeof(msg), TRB_TR_IDT);
    dbc_writel(t, DBC_DCDB, DB_IN << 8);

    g_assert_true(wait_event(t, &parameter, &status, &control));
    g_assert_cmpuint(event_type(control), ==, ER_TRANSFER);
    g_assert_cmpuint(event_cc(status), ==, CC_SUCCESS);
    g_assert_cmpuint(parameter, ==, trb);

    for (i = 0; i < 1000 && total < sizeof(msg); i++) {
        ssize_t done = recv(t->sock, got + total, sizeof(msg) - total,
                            MSG_DONTWAIT);
        if (done > 0) {
            total += done;
            continue;
        }
        g_usleep(1000);
    }
    g_assert_cmpuint(total, ==, sizeof(msg));
    g_assert_cmpmem(got, sizeof(got), msg, sizeof(msg));
}

/* a stream of transfers, to exercise ring wrap and the link TRB */
static void dbc_test_stream(DbCTest *t)
{
    uint64_t parameter;
    uint32_t status, control;
    char msg[16];
    char got[16];
    int i;

    for (i = 0; i < 12; i++) {
        size_t total = 0;
        int j;

        snprintf(msg, sizeof(msg), "packet %05d", i);
        qtest_memwrite(t->qts, ADDR_BUF_IN, msg, sizeof(msg));
        dbc_post_in(t, ADDR_BUF_IN, sizeof(msg), 0);
        dbc_writel(t, DBC_DCDB, DB_IN << 8);

        g_assert_true(wait_event(t, &parameter, &status, &control));
        g_assert_cmpuint(event_type(control), ==, ER_TRANSFER);
        g_assert_cmpuint(event_cc(status), ==, CC_SUCCESS);

        for (j = 0; j < 1000 && total < sizeof(msg); j++) {
            ssize_t done = recv(t->sock, got + total, sizeof(msg) - total,
                                MSG_DONTWAIT);
            if (done > 0) {
                total += done;
                continue;
            }
            g_usleep(1000);
        }
        g_assert_cmpuint(total, ==, sizeof(msg));
        g_assert_cmpstr(got, ==, msg);
    }
}

/*
 * A reset from the debug host: the DbC-Configured -> DbC-Resetting ->
 * DbC-Enabled -> DbC-Configured loop, which is the reconnect path.
 */
static void dbc_test_host_reset(DbCTest *t)
{
    uint64_t parameter;
    uint32_t status, control, dcportsc;
    int i;

    g_assert_cmpint(send(t->ctrl, "r", 1, 0), ==, 1);

    for (i = 0; i < 1000; i++) {
        dcportsc = dbc_readl(t, DBC_DCPORTSC);
        if (dcportsc & DCPORTSC_PR) {
            break;
        }
        g_usleep(1000);
    }
    /* DbC-Resetting: PR asserted, port disabled, no longer configured */
    g_assert_cmpuint(dcportsc & DCPORTSC_PR, ==, DCPORTSC_PR);
    g_assert_cmpuint(dcportsc & DCPORTSC_PED, ==, 0);
    g_assert_cmpuint(dbc_readl(t, DBC_DCCTRL) & DCCTRL_DCR, ==, 0);
    /* leaving Configured sets DRC, which kills the doorbell */
    g_assert_cmpuint(dbc_readl(t, DBC_DCCTRL) & DCCTRL_DRC, ==, DCCTRL_DRC);

    /* reset completes: PR falls, PED reasserts, PRC is set */
    qtest_clock_step_next(t->qts);
    dcportsc = dbc_readl(t, DBC_DCPORTSC);
    g_assert_cmpuint(dcportsc & DCPORTSC_PR, ==, 0);
    g_assert_cmpuint(dcportsc & DCPORTSC_PED, ==, DCPORTSC_PED);
    g_assert_cmpuint(dcportsc & DCPORTSC_PRC, ==, DCPORTSC_PRC);
    g_assert_true(wait_event(t, &parameter, &status, &control));
    g_assert_cmpuint(event_type(control), ==, ER_PORT_STATUS);

    /* and the debug host configures us again */
    qtest_clock_step_next(t->qts);
    g_assert_cmpuint(dbc_readl(t, DBC_DCCTRL) & DCCTRL_DCR, ==, DCCTRL_DCR);

    /*
     * The transport only comes back once software acknowledges DRC.  Note
     * the read-modify-write: PED is a plain RW bit, so acknowledging a
     * change bit with a bare write would clear PED and disable the port.
     */
    dbc_writel(t, DBC_DCPORTSC,
               (dbc_readl(t, DBC_DCPORTSC) & DCPORTSC_PED) | DCPORTSC_PRC);
    dbc_writel(t, DBC_DCCTRL, dbc_readl(t, DBC_DCCTRL) | DCCTRL_DRC);
    g_assert_cmpuint(dbc_readl(t, DBC_DCCTRL) & DCCTRL_DRC, ==, 0);

    qtest_memwrite(t->qts, ADDR_BUF_IN, "after-reset", 12);
    dbc_post_in(t, ADDR_BUF_IN, 12, 0);
    dbc_writel(t, DBC_DCDB, DB_IN << 8);
    g_assert_true(wait_event(t, &parameter, &status, &control));
    g_assert_cmpuint(event_type(control), ==, ER_TRANSFER);
    g_assert_cmpuint(event_cc(status), ==, CC_SUCCESS);
}

/*
 * A malformed TRB halts the endpoint: the offending buffer comes back with
 * Stall Error, HIT latches, and nothing moves again until the debug host
 * sends ClearFeature(ENDPOINT_HALT).
 */
static void dbc_test_halt(DbCTest *t)
{
    uint64_t parameter, trb;
    uint32_t status, control, epctx;
    int i;

    /* a TR_NOOP where the DbC expects a Normal TRB */
    trb = ADDR_RING_IN + t->in_enq * 16;
    write_trb(t, trb, 0, 0, (8 << TRB_TYPE_SHIFT) | (t->in_cycle ? TRB_C : 0));
    t->in_enq++;
    dbc_writel(t, DBC_DCDB, DB_IN << 8);

    g_assert_true(wait_event(t, &parameter, &status, &control));
    g_assert_cmpuint(event_type(control), ==, ER_TRANSFER);
    g_assert_cmpuint(event_epid(control), ==, EPID_IN);
    g_assert_cmpuint(event_cc(status), ==, CC_STALL_ERROR);
    g_assert_cmpuint(parameter, ==, trb);

    /* HIT latches, and the endpoint context says Halted */
    g_assert_cmpuint(dbc_readl(t, DBC_DCCTRL) & DCCTRL_HIT, ==, DCCTRL_HIT);
    epctx = qtest_readl(t->qts, ADDR_CTX + 0x80) & 0x7;
    g_assert_cmpuint(epctx, ==, 2);

    /* a good TRB now goes nowhere: the pipe is halted */
    qtest_memwrite(t->qts, ADDR_BUF_IN, "ignored", 8);
    dbc_post_in(t, ADDR_BUF_IN, 8, 0);
    dbc_writel(t, DBC_DCDB, DB_IN << 8);
    g_assert_false(wait_event(t, &parameter, &status, &control));

    /* the debug host clears the halt; the endpoint parks in Stopped */
    g_assert_cmpint(send(t->ctrl, "c", 1, 0), ==, 1);
    for (i = 0; i < 1000; i++) {
        if (!(dbc_readl(t, DBC_DCCTRL) & DCCTRL_HIT)) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_cmpuint(dbc_readl(t, DBC_DCCTRL) & DCCTRL_HIT, ==, 0);
    epctx = qtest_readl(t->qts, ADDR_CTX + 0x80) & 0x7;
    g_assert_cmpuint(epctx, ==, 3);

    /*
     * Recovery is software's job: point the endpoint context at the TRB it
     * wants retried, then ring the doorbell.
     */
    qtest_writeq(t->qts, ADDR_CTX + 0x80 + 8,
                 (ADDR_RING_IN + (t->in_enq - 1) * 16) |
                 (t->in_cycle ? 1 : 0));
    dbc_writel(t, DBC_DCDB, DB_IN << 8);
    g_assert_true(wait_event(t, &parameter, &status, &control));
    g_assert_cmpuint(event_type(control), ==, ER_TRANSFER);
    g_assert_cmpuint(event_cc(status), ==, CC_SUCCESS);
}

/* disabling the capability parks the endpoints and drops the link */
static void dbc_test_disable(DbCTest *t)
{
    uint32_t dcctrl;

    dbc_writel(t, DBC_DCCTRL, 0);

    dcctrl = dbc_readl(t, DBC_DCCTRL);
    g_assert_cmpuint(dcctrl & DCCTRL_DCE, ==, 0);
    g_assert_cmpuint(dcctrl & DCCTRL_DCR, ==, 0);
    g_assert_cmpuint(dbc_readl(t, DBC_DCPORTSC) & DCPORTSC_CCS, ==, 0);

    /* the endpoint contexts are handed back stopped */
    g_assert_cmpuint(qtest_readl(t->qts, ADDR_CTX + 0x40) & 0x7, ==, 3);
    g_assert_cmpuint(qtest_readl(t->qts, ADDR_CTX + 0x80) & 0x7, ==, 3);
}

static void test_dbc(void)
{
    g_autofree char *sock_dir = NULL;
    g_autofree char *sock_path = NULL;
    g_autofree char *ctrl_path = NULL;
    DbCTest t = {};
    int listen_fd, ctrl_fd;

    sock_dir = g_dir_make_tmp("qtest-dbc-XXXXXX", NULL);
    g_assert_nonnull(sock_dir);
    sock_path = g_strdup_printf("%s/dbc.sock", sock_dir);
    ctrl_path = g_strdup_printf("%s/ctrl.sock", sock_dir);

    listen_fd = unix_listen(sock_path, NULL);
    g_assert_cmpint(listen_fd, >=, 0);
    ctrl_fd = unix_listen(ctrl_path, NULL);
    g_assert_cmpint(ctrl_fd, >=, 0);

    t.qts = qtest_initf("-machine pc -m 128 "
                        "-chardev socket,id=dbc0,path=%s "
                        "-chardev socket,id=ctrl0,path=%s "
                        "-device qemu-xhci,id=xhci,addr=04.0,"
                        "dbc-chardev=dbc0,dbc-control-chardev=ctrl0",
                        sock_path, ctrl_path);

    t.sock = accept(listen_fd, NULL, NULL);
    g_assert_cmpint(t.sock, >=, 0);
    t.ctrl = accept(ctrl_fd, NULL, NULL);
    g_assert_cmpint(t.ctrl, >=, 0);
    close(listen_fd);
    close(ctrl_fd);
    unlink(sock_path);
    unlink(ctrl_path);

    t.bus = qpci_new_pc(t.qts, NULL);
    t.dev = qpci_device_find(t.bus, QPCI_DEVFN(0x4, 0x0));
    g_assert_nonnull(t.dev);
    qpci_device_enable(t.dev);
    t.bar = qpci_iomap(t.dev, 0, NULL);

    dbc_setup(&t);
    dbc_test_out(&t);
    dbc_test_in(&t);
    dbc_test_in_immediate(&t);
    dbc_test_stream(&t);
    dbc_test_port_disable(&t);
    dbc_test_host_reset(&t);
    dbc_test_halt(&t);
    dbc_test_disable(&t);

    close(t.sock);
    close(t.ctrl);
    g_free(t.dev);
    qpci_free_pc(t.bus);
    qtest_quit(t.qts);
    rmdir(sock_dir);
}

/* without a chardev and without dbc=on the capability must stay hidden */
static void test_no_dbc(void)
{
    DbCTest t = {};

    t.qts = qtest_initf("-machine pc -m 128 "
                        "-device qemu-xhci,id=xhci,addr=04.0");
    t.bus = qpci_new_pc(t.qts, NULL);
    t.dev = qpci_device_find(t.bus, QPCI_DEVFN(0x4, 0x0));
    g_assert_nonnull(t.dev);
    qpci_device_enable(t.dev);
    t.bar = qpci_iomap(t.dev, 0, NULL);

    g_assert_cmpuint(find_dbc_cap(&t), ==, 0);
    /* CAPLENGTH stays at its historic value when the DbC is absent */
    g_assert_cmpuint(qpci_io_readl(t.dev, t.bar, 0) & 0xff, ==, 0x40);

    g_free(t.dev);
    qpci_free_pc(t.bus);
    qtest_quit(t.qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/xhci/dbc/absent", test_no_dbc);
    qtest_add_func("/xhci/dbc/transport", test_dbc);

    return g_test_run();
}
