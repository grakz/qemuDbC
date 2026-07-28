# Findings note — QEMU xHCI Debug Capability (DbC) model

Returned to Tosix-core under DR-161 (findings note only; the code stays in
this GPL repo).  Task: DR-177, the cloud-GPL work item that makes the Tosix
DbC transport exercisable in a VM.

## 1. Upstream base

| | |
|---|---|
| Project | QEMU, `gitlab.com/qemu-project/qemu` |
| Tag | `v10.2.4` (branch `stable-10.2`) |
| Commit | `3e0bcba` — *"Update version for 10.2.4 release"* |
| Work branch | `claude/qemu-xhci-dbc-model-0xrnue` (5 commits on that tag) |
| Binaries branch | `claude/qemu-xhci-dbc-binaries-0xrnue` |

## 2. Register-by-register status

The capability is chained onto the xHCI extended-capability list as ID
`0x0A`, behind the two Supported Protocol capabilities, at BAR offset
`0x040`.

| Register | Off | Status |
|---|---|---|
| DCID | 0x00 | RO. Cap ID 0x0A, next-pointer 0 (end of list), DCERST Max = 0 (one segment) |
| DCDB | 0x04 | Doorbell, reads 0. Target **0 = OUT ring, 1 = IN ring**; 2-255 are Reserved and are rejected with a logged guest error. Disabled while DCCTRL.DRC is set |
| DCERSTSZ | 0x08 | RW. Only the value 1 is honoured; anything else is a logged guest error |
| DCERSTBA | 0x10/0x14 | RW 64-bit, bits 3:0 RsvdP. ERST parsed on DCE 0→1 |
| DCERDP | 0x18/0x1C | RW 64-bit incl. the DESI field. Drives event-ring-full back-pressure; a write re-kicks both pipes |
| DCCTRL | 0x20 | DCE/LSE (RW), DRC (RW1C, set only when DCR clears), HOT/HIT (RW1S, always 0 - no halt modelled), DCR + device address + max-burst (RO) |
| DCST | 0x24 | RO. Event Ring Not Empty, SBR, and the debug port number - reported as 0 until a host is attached |
| DCPORTSC | 0x28 | CCS/PLS/speed RO, **PED RW**, CSC/PRC/PLC/CEC RW1C. Bit layout mirrors the operational PORTSC |
| DCCP | 0x30/0x34 | RW 64-bit. DbCC read on enable |
| DCDDI1/2 | 0x38/0x3C | RW. Stored, reported back; not otherwise interpreted |

Also implemented: the DbC **event ring** (single segment, own cycle state,
ERDP-based overrun back-pressure), the **two bulk endpoint contexts**
(loaded from DbCC+0x40 and DbCC+0x80, EP state written back as
RUNNING/STOPPED, dequeue pointer returned on teardown), Normal and Link
TRBs with toggle-cycle, immediate data (IDT), short-packet completion, and
Transfer Events carrying endpoint IDs 2 (OUT) and 3 (IN).

Host bridge: `-device qemu-xhci,dbc-chardev=<id>`.  Chardev peer connected
= cable plugged in; `DCPORTSC.CCS` and then `DCCTRL.DCR` follow the peer,
and a disconnect drops the link and posts a Port Status Change Event.

**Note for the Tosix DbC driver:** the model publishes the event TRB's
control dword (and so its cycle bit) *after* the rest of the TRB, which is
what the architecture requires for a polled ring.  A driver that reads the
TRB fields before checking the cycle bit will tear — the first cut of our
own test did exactly that and failed about one run in three.  Read the
cycle bit first.

## 3. Build status

| Target | Result |
|---|---|
| Linux x86-64 (gcc 13.3, Ubuntu 24.04) | clean, no warnings |
| Windows x86-64 (mingw-w64 cross, glib/pixman from MSYS2) | clean; the .exe boots a guest and round-trips DbC bytes (verified under Wine 9.0) |
| `scripts/checkpatch.pl` | 0 errors on the four new-content commits; the pure-move commit repeats pre-existing `<<` spacing complaints on the lines it moves |
| Conformance | Cross-checked against xHCI 1.2c section 7.6 - see section 6 |

## 4. Validation — and whether bytes round-tripped

**Yes, in both directions, in three independent settings.**

1. **qtest** (`tests/qtest/usb-hcd-xhci-dbc-test.c`, in-tree, gated):
   discovers the capability, builds ERST/event ring/DbCC/two transfer rings
   in guest memory, round-trips payloads both ways through a unix-socket
   chardev, wraps the IN ring over a Link TRB twelve times, checks the
   RW1C change bits, the immediate-data (IDT) path and the teardown. Also
   asserts that with no DbC asked for, the capability is absent and
   CAPLENGTH stays 0x40. Ran 10/10 clean; an earlier cut caught a genuine
   TRB-tearing race, see the note in section 2.

2. **Bare probe inside a real Debian 12 guest** (kernel 6.1.0-51-amd64):
   a userspace program unbinds `xhci_hcd`, maps BAR0 through sysfs, walks
   the extended-capability list, and DMAs to real guest-physical pages
   resolved through `/proc/self/pagemap`. Output:

   ```
   CAPLENGTH = 0x80, HCIVERSION = 0x0100
     ext cap id 0x02 at 0x020 / 0x02 at 0x030 / 0x0a at 0x040
   dbc running (DCR)            PASS dcctrl=0x81000013
   port connected (CCS/PED)     PASS dcportsc=0x00021003
   port status change event     PASS type=34
   IN transfer event            PASS cc=1 epid=3 residual=0
   OUT transfer event           PASS cc=13 epid=2 residual=43
   host payload in guest ram    PASS b'DBC_HOST_TO_GUEST_OK'
   12 transfers across wrap     PASS
   PROBE_RESULT: OK (0 failures)
   ```

   and on the host end of the cable: `DBC_GUEST_TO_HOST_OK` followed by the
   twelve streamed packets.

3. **The shipped Windows binary**, running under Wine, boots the same
   Debian guest and passes the same probe — the DbC path is not
   Linux-host-specific.

4. **No-regression boots**: Alpine 3.21 (kernel 6.12.13) and Debian 12 both
   boot with the DbC advertised; `xhci_hcd` binds, `hcc params 0x00087001`
   unchanged, and USB keyboard and tablet enumerate normally.

**What we could not do:** drive the model with the *stock Linux DbC driver*.
Both distributions ship `# CONFIG_USB_XHCI_DBGCAP is not set` (verified from
`/boot/config-$(uname -r)`), so no `dbc` sysfs node and no `/dev/ttyDBC0` is
available to enable. This is a distro-kernel gap, not a discovery failure —
the probe above proves the capability is visible and driveable from inside
the same guests. Testing against the in-tree Linux driver needs a kernel
built with `CONFIG_USB_XHCI_DBGCAP=y`.

## 5. Hypervisor exit cost, and the driver rules that protect it

Raised from the Tosix side: QEMU's emulated 16550 serial port takes a
hypervisor exit for every byte written, which under WHPX on Windows makes
it painfully slow.  Does the DbC transport have the same problem?

**No, and by a wide margin — but the driver has to not throw it away.**

The 16550 is a register interface: each byte is an `out` to the transmit
holding register, and drivers normally poll the line status register for
THRE before each one, so it costs roughly two exits *per byte* and stalls
the vCPU on each.  The DbC is a DMA ring interface, and almost all of the
work happens in guest RAM where nothing traps:

| Step | Where it happens | Exits |
|---|---|---|
| Build TRBs, write the payload | guest RAM | 0 |
| Ring `DCDB` | MMIO | **1** |
| Model reads the TRBs and payload, writes the chardev | host-side DMA | 0 |
| Model writes the Transfer Events | guest RAM | 0 |
| Driver notices completion via the cycle bit | guest RAM | 0 |
| Advance `DCERDP` | MMIO | **1** |

Completion detection is the step that ruins the serial port, and here it is
free.

**Measured**, with a batch phase added to the in-guest probe of section 4
(32 TRBs of 256 bytes = 8192 bytes behind a single doorbell) and every DbC
register access traced: the steady-state data path cost **two MMIO
accesses — one `DCDB` write and one `DCERDP` write — for 8192 bytes**, and
**zero** accesses to observe the 32 completions.  The equivalent 16550
traffic for 8 KB is upwards of 16000 exits.  The ratio grows with batch
size: the TRB length field is 17 bits (128 KB - 1 per TRB) and the model
drains up to 256 TRBs per doorbell.

(Note when reading traces of your own: QEMU does *not* read-modify-write
these registers — one MMIO write is one model access, confirmed against the
qtest, which issues single writes with no guest CPU involved.  The probe's
Python/ctypes accessor happens to load before it stores, which doubles the
count in its trace; a C `writel()` does not.)

### Rules for the Tosix DbC driver

1. **Poll the cycle bit in the event ring, never `DCST.ER`.** The cycle bit
   is in guest RAM and costs nothing; `DCST` is MMIO and costs one exit per
   poll. Read the cycle bit *before* the rest of the TRB — see the tearing
   note in section 2.
2. **Keep `DCCTRL` and `DCPORTSC` reads in the enable path only**, out of
   the data path. Polling either one per transfer reintroduces the serial
   port's problem.
3. **One doorbell per batch, not per TRB.** Queue everything that is ready,
   then ring once.
4. **Update `DCERDP` once per batch of consumed events, not per event.**

Break any of these and the transport slides back toward serial-like
behaviour without any visible error.

### Can it be literally zero exits?

Not for the doorbell.  Any guest MMIO store traps by construction; QEMU
cannot make the guest's instruction not trap.  The mechanisms that avoid
the expensive *userspace* leg — KVM coalesced MMIO and ioeventfd — are
KVM-only, and WHPX always goes to userspace.

The only true zero-exit route would be to back the doorbell page with plain
RAM and have QEMU poll it; a hardware-correct driver would be unchanged,
since a doorbell write is fire-and-forget.  For the DbC that is blocked by
layout: `DCDB` sits at cap+0x04, sharing a 4 KB page with `CAPLENGTH`,
`HCCPARAMS` and the operational registers, all of which must trap.  It
would need the whole extended-capability list relocated onto its own page
(`HCCPARAMS.xECP` is a 16-bit dword offset, so it can reach, but the BAR is
0x4000 with MSI-X at 0x3000/0x3800 and would have to grow), a RAM-backed
page, and a polling thread.  Not upstreamable as default behaviour;
defensible as an opt-in later if two exits per batch ever turns out to
matter.  It should not.

WHPX is compiled into the shipped Windows binary, so `-accel whpx` works
without rebuilding.

## 6. Cross-check against xHCI 1.2c section 7.6

The register-level spec (Intel document 868295, revision 1.2c, section 7.6)
was read against the implementation after the first cut. Every finding
below was confirmed verbatim in the source PDF, not only in the markdown
extract. **Nine divergences were found and fixed**; the model in the branch
is the corrected one.

| # | Divergence in the first cut | Spec | Fixed to |
|---|---|---|---|
| 1 | HCRST left the DbC running, and the commit message claimed the spec required that | 7.6.6.1: HCRST drives the port to DbC-Off. 7.6.8.5: DCST.SBR selects between the two behaviours | HCRST resets the DbC by default and SBR reads 0; `dbc-sbr=on` gives the survive-HCRST behaviour and reads 1 |
| 2 | Doorbell accepted targets 0-3 | 7.6.8.2: 0 = OUT, 1 = IN, 2-255 Reserved | Only 0 and 1; reserved targets logged and ignored |
| 3 | Doorbell worked regardless of DRC | 7.6.8.4: "While this bit is '1' the DCDB is disabled" | Doorbell ignored while DRC is set |
| 4 | DRC set on every DCR transition | 7.6.8.4: set when DCR is *cleared* | Set only on the 1→0 transition |
| 5 | Port Status Change Event carried Port ID 1 | 7.6.4.2: "always '0' ... on the Debug Capability's Event Ring" | Port ID 0 |
| 6 | An event per port change | 7.6.4.2: events only on a 0→1 edge of the OR of the four change bits (DCPSCEG) | Edge-triggered |
| 7 | DCST reported a port number unconditionally | 7.6.6: Debug Port Number is 0 in DbC-Off and DbC-Disconnected | 0 until a host is attached |
| 8 | DCPORTSC.PED treated as read-only | 7.6.8.6: PED is RW; clearing it enters DbC-Disabled | PED writable; clearing it exits DbC-Configured |
| 9 | DCERSTBA masked bits 5:0; DCERDP dropped DESI; DMA faults reported Data Buffer Error | 7.6.8.3.2 (3:0 RsvdP), 7.6.8.3.3 (DESI is RW), 7.6.4.2 (allowed completion codes) | Masks corrected; DESI readable; TRB Error used |

Also corrected on the way: the model now publishes the event TRB's control
dword after the rest of the TRB, and completes an in-flight buffer with USB
Transaction Error when leaving DbC-Configured (7.6.4.4).

**Two of these bit our own test as soon as they were enforced**, which is
the point of the exercise:

- The test rang the doorbell with the endpoint DCI (2 and 3) instead of the
  doorbell targets (0 and 1). These are **different numbering spaces** -
  DCDB takes 0/1, while transfer events report the DCI, 2 for EP 1 OUT and
  3 for EP 1 IN. Worth checking in the Tosix driver.
- On re-enable after a disable, DRC is still set, so the doorbell is dead
  until software clears it. A driver that re-enables and rings without
  clearing DRC will hang, and will hang on real silicon too.

### Interrupts: there is nothing to model

Confirmed while checking: **the DbC has no interrupt mechanism at all.**
It has no interrupter registers - no IMAN, no IMOD, no ERSTBA-associated
interrupter - and section 7.6.4.1 instructs software to "periodically poll
the Event Ring Not Empty bit in the DCST register, or evaluate the DbC
Event Ring for change in the Event Ring Enqueue Pointer". A sweep of the
whole of 7.6 in the PDF turns up no interrupt assertion for the DbC; the
only "Interrupter" text nearby belongs to 7.7, the xHCI-IOV capability.

So poll-only is the architecture, not a modelling shortcut, and the caveat
about an untested interrupt-driven DbC for live/userspace use does not
apply - there is no such mode to test. Reading DCST.ER is a legitimate way
to poll per 7.6.4.1; it is simply the expensive one under a hypervisor
(section 5).

### The port state machine is modelled whole

Section 7.6.6 is a *register* state machine - only the LTSSM beneath it is
electrical - so all seven states are implemented: Off, Disconnected,
Enabled, Disabled, Configured, Resetting and Error, with the DCPORTSC
change bits and the 7.6.4.4 configured-exit behaviour. Enumeration and
reset completion take a short interval of virtual time, so DbC-Enabled and
DbC-Resetting are states a driver can observe rather than instants it can
never catch - a driver that treats CCS as "ready" now trips in the VM.

The debug host's own actions - bus reset, SET_CONFIGURATION(0),
ClearFeature(ENDPOINT_HALT), link and config errors - cannot be carried in
band by a byte pipe, so they are injected on a second chardev,
`dbc-control-chardev`, one character each. That is what makes DbC-Resetting
and DbC-Error reachable, and it is how the reconnect loop is rehearsed:
Configured -> Resetting -> Enabled -> Configured, with the doorbell dead
until DRC is acknowledged.

**A fifth rule, from 7.6.7.4:** the DbC does not look at either transfer
ring until software rings that ring's doorbell - being DbC-Configured is
not enough, and the same applies after a port reset. The model enforces
this, which caught our own probe handing the DbC a ring it had not
finished initialising: the DbC did exactly the right thing and halted the
endpoint on the garbage TRB it found.

**A fourth driver hazard**, found while building this: `DCPORTSC.PED` is a
plain RW bit, so acknowledging a change bit with a bare write clears PED
and disables the port. Acknowledgements must be read-modify-write. Our own
test did it wrong first.

### Endpoint halt and recovery, and DbCIC checking

Both of the S-sized bucket-B items are now in.

**Halt (7.6.4.3).** The DbC raises HOT/HIT itself on a TRB error, a bad
buffer pointer or an over-long immediate-data TRB, and software can raise
them by writing HOT/HIT (they are RW1S). Either way the in-flight buffer
is completed with **Stall Error** whether or not IOC was set, the halt bit
latches, and the endpoint context is written back as **Halted**. The pipe
stays dead until the debug host sends ClearFeature(ENDPOINT_HALT) on the
control chardev, which moves it to **Stopped**; software then sets the
context's TR Dequeue Pointer to whatever it wants retried and rings the
doorbell, and the DbC picks up from there. That is the full recovery loop
a driver has to implement, and it is now exercised by the qtest.

**DbCIC (7.6.9.1).** The info context is read and checked on enable -
still no enumeration, so the strings are never handed to anyone, but a
descriptor the hardware would choke on is now a logged guest error: a
non-zero declared length with a null pointer, a `bLength` that disagrees
with the declared length, a `bDescriptorType` that is not STRING, or a
missing string 0. Note for anyone taking this further to real
enumeration: the string-index sentences in 7.6.9.1 sit one table away from
the fields they describe in the PDF's text flow, so a naive reading shifts
every index by one; the correct assignment agrees with
iManufacturer/iProduct/iSerialNumber = 01h/02h/03h.

### Deliberately still not modelled

- **Real descriptor enumeration** - the DbC never presents itself to a USB
  host, so descriptor *content* bugs beyond the checks above do not
  surface. Effort: **M**.
- **The LTSSM.** Link training, U1/U2/U3 transitions and the timeouts that
  drive DbC-Error arise in the electrical layer. Their register-visible
  consequences can be injected over the control chardev; the layer itself
  cannot be modelled and stays bucket A.

### Spec-extract caveats that did *not* affect the model

Everything encoded above came from the register bit-definition tables and
the 7.6.6.x prose, both of which extract cleanly and were re-checked
against the PDF. The known-lossy parts of the extract - Figure 7-8's
transition graph, the inferred (DCE, CCS, PED, PR, DCR) tuple, the
hand-retabulated descriptor tables in file 08, and Figure 7-10's
end-of-structure label - are not load-bearing for anything implemented
here. They would become load-bearing if descriptor enumeration is added.

## 7. Known gaps versus real DbC

Bearing on the Phase-0 obligation:

* **No PTL025 behaviour.** Deep-sleep (S4/S5/G3) survival is not modelled at
  all. This is the specific erratum the reference unit has, and the model
  says nothing about it.
* **No port mapping.** The model does not consume a root port; `DCST`
  reports a fixed, cosmetic debug port number. Which physical connector the
  DbC lands on — a real Phase-0 unknown — is untestable here.
* **No electrical or link-training behaviour.** The link comes up
  instantly and is always SuperSpeed; no polling, recovery, compliance or
  hot-reset states, no U1/U2/U3.
* **No USB enumeration.** The DbCIC string descriptors are ignored; nothing
  on the emulated far end reads them. Descriptor-level bugs in the Tosix
  driver will not be caught.
* **No interrupts.** DbC events never assert the xHCI interrupt; the driver
  must poll. Real DbC can interrupt, so an interrupt-driven Tosix DbC path
  is not exercised.
* **No endpoint halt/stall path.** `DCCTRL.HOT`/`HIT` are accepted and read
  back zero; the model never stalls, so error recovery is untested.
* **Single event-ring segment**, matching the primary event ring of the same
  QEMU model, whereas hardware may allow more.
* **Timing is nothing like hardware.** Transfers complete synchronously with
  the doorbell write; there is no bus turnaround, no burst behaviour, no
  back-pressure from a real link.

So, per the brief: this **reduces but does not close** the Phase-0
obligation. Laws 1–2 still bind — DbC remains a real-silicon bring-up item
on the reference unit, and the items above are exactly the ones that will
first meet hardware unrehearsed.

## 8. Where the branches and patches live

Repository `grakz/qemuDbC`:

* `claude/qemu-xhci-dbc-model-0xrnue` — the source series:
  1. `hw/usb/hcd-xhci: move shared TRB definitions to the header`
  2. `hw/usb/hcd-xhci: split the host controller reset out of the device reset`
  3. `hw/usb: model the xHCI Debug Capability (DbC)`
  4. `tests/qtest: cover the xHCI Debug Capability`
  5. `docs/system: document the xHCI Debug Capability`

  `git format-patch 3e0bcba..claude/qemu-xhci-dbc-model-0xrnue` produces the
  series as-is (the clone carries no tags, hence the sha); it carries Signed-off-by lines and is shaped for upstream
  submission.

* `claude/qemu-xhci-dbc-binaries-0xrnue` — the same series plus `dbc-dist/`:
  ready-to-run Linux (`.tar.xz`) and Windows (`.zip`) bundles, `SHA256SUMS`,
  the packaging script and usage notes. Neither bundle needs the source
  tree.

Licensing: QEMU is GPL v2; the two new model files are LGPL-2.1-or-later to
match `hw/usb/hcd-xhci.c`, which they are part of. All of it stays on the
GPL side of the DR-161 boundary — only this note crosses.
