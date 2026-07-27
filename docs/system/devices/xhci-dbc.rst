.. SPDX-License-Identifier: GPL-2.0-or-later

xHCI Debug Capability (DbC)
===========================

The xHCI controllers (``qemu-xhci``, ``nec-usb-xhci``, ``sysbus-xhci``) can
advertise the xHCI *Debug Capability*, extended capability ID 0x0a.  On real
hardware the DbC turns one root port into a USB debug device, so that a
second machine plugged into that port sees a simple two-endpoint bulk device
and can be used as a debug console or debugger transport.  Its selling point
is that it is driven directly out of the controller's register block, and is
independent of the driver stack of the operating system being debugged.

QEMU models the software side of that: the register block, the DbC event
ring, the debug capability context and the two bulk transfer rings.  The
two bulk pipes are bridged to a chardev, which stands in for the machine at
the far end of the debug cable.

Usage
-----

Attach a chardev to the controller with the ``dbc-chardev`` property::

  qemu-system-x86_64 \
      -chardev socket,id=dbc0,path=/tmp/dbc.sock,server=on,wait=off \
      -device qemu-xhci,id=xhci,dbc-chardev=dbc0

Anything the guest sends on the DbC bulk IN endpoint appears on the chardev,
and anything written to the chardev is delivered to the guest through the
bulk OUT endpoint.  The chardev being connected is what the guest sees as
the debug cable being plugged in: ``DCPORTSC.CCS`` is asserted, and
``DCCTRL.DCR`` follows, only while a peer is attached.  Disconnecting the
peer drops the link and posts a port status change event, exactly as
unplugging the cable would.

The capability is only advertised when it is asked for, so that the guest
visible register layout is unchanged for everyone else.  Setting
``dbc-chardev`` implies it; ``dbc=on`` advertises the capability without a
backend, which is only useful for testing the discovery path::

  -device qemu-xhci,dbc=on

The debug host's control plane
------------------------------

Some of the port state machine of xHCI 1.2 section 7.6.6 is driven by
things the debug host does over the USB link - bus resets,
``SET_CONFIGURATION(0)``, ``ClearFeature(ENDPOINT_HALT)``, link errors -
which a byte-pipe chardev cannot carry in band.  ``dbc-control-chardev``
is a second chardev on which those events are injected, one character
each::

  -chardev socket,id=ctrl0,path=/tmp/dbc-ctrl.sock,server=on,wait=off \
  -device qemu-xhci,dbc-chardev=dbc0,dbc-control-chardev=ctrl0

===========  ==========================================================
Character    Event
===========  ==========================================================
``r``/``w``  Hot or Warm Reset: enter DbC-Resetting, then re-enumerate
``d``        ``SET_CONFIGURATION(0)``: leave DbC-Configured
``c``        ``ClearFeature(ENDPOINT_HALT)``: clear ``HOT``/``HIT``
``e``        Link error: enter DbC-Error and set ``PLC``
``x``        Port configuration error: enter DbC-Disabled and set ``CEC``
``k``        The debug host retrains the link and recovers the port
===========  ==========================================================

Without this chardev the DbC still reaches DbC-Off, DbC-Disconnected,
DbC-Enabled, DbC-Disabled and DbC-Configured; the control plane is what
makes DbC-Resetting and DbC-Error reachable as well.

Enumeration by the debug host, and the completion of a reset, each take a
short interval of virtual time rather than happening instantly, so that
DbC-Enabled and DbC-Resetting are states a driver can observe rather than
instants it can never catch.

Host controller reset
---------------------

Whether a host controller reset (``USBCMD.HCRST``) also resets the Debug
Capability is a property of the implementation, reported to software in
``DCST.SBR``.  By default this model takes the behaviour of xHCI 1.2
section 7.6.6.1 - HCRST drives the DbC port to DbC-Off - and reports
``SBR`` as 0.  ``dbc-sbr=on`` selects the other documented behaviour, where
only a chip or system bus reset resets the DbC and a debug session survives
the guest re-initialising the controller, and reports ``SBR`` as 1::

  -device qemu-xhci,dbc-chardev=dbc0,dbc-sbr=on

A driver that reads ``SBR`` and handles both is portable across parts;
having both available here is what lets it be tested.

Because the DbC register block is chained onto the extended capability list
directly behind the two Supported Protocol capabilities, the operational
registers move out of its way and ``CAPLENGTH`` reads 0x80 instead of 0x40
when the capability is present.  Guests locate the operational registers
through ``CAPLENGTH``, so this is transparent.

Guest support
-------------

Linux implements the DbC under ``CONFIG_USB_XHCI_DBGCAP``, which exposes a
``dbc`` attribute on the controller's device in sysfs and, once enabled,
a ``/dev/ttyDBC0`` character device::

  echo enable > /sys/bus/pci/devices/0000:00:04.0/dbc

Note that several distributions ship that option turned off, in which case
the capability is simply ignored by the guest.

Implemented registers
---------------------

All of ``DCID``, ``DCDB``, ``DCERSTSZ``, ``DCERSTBA``, ``DCERDP``,
``DCCTRL``, ``DCST``, ``DCPORTSC``, ``DCCP``, ``DCDDI1`` and ``DCDDI2``
are implemented.  The register block is accessed 32 bits at a time.

The transfer rings are untouched until software rings the doorbell.  Being
configured is not enough: per section 7.6.7.4 the endpoints stay quiet
"until software notifies the DbC that the respective Transfer Rings have
been initialized by ringing their doorbells".  That applies after a port
reset as well as at first configuration.

Endpoints halt on error, as they do on hardware.  A malformed TRB, a bad
buffer pointer or a write to ``DCCTRL.HOT``/``HIT`` completes the offending
buffer with Stall Error whether or not it asked for an interrupt, latches
the halt bit, and writes the endpoint context back as Halted.  Nothing
moves on that pipe again until the debug host sends
``ClearFeature(ENDPOINT_HALT)`` over the control chardev, which parks the
endpoint in Stopped; software then points the context's TR Dequeue Pointer
at whatever it wants retried and rings the doorbell.

Two more things bite drivers here.  ``DCPORTSC.PED`` is a plain RW bit, so
acknowledging a change bit with a bare write clears ``PED`` and disables
the port - acknowledgements must be read-modify-write.  And while
``DCCTRL.DRC`` is set the doorbell is dead, so a driver that re-enables
after a reset without clearing ``DRC`` will queue TRBs that are never
picked up.

Note that the ``DCDB`` doorbell targets and the endpoint IDs reported in
transfer events are different numbering spaces: the doorbell takes 0 for
the OUT ring and 1 for the IN ring, while transfer events carry the
ordinary xHCI DCI, 2 for EP 1 OUT and 3 for EP 1 IN.  Reserved doorbell
targets are ignored and logged as guest errors.

Limitations
-----------

* Only a single event ring segment is supported, so ``DCID`` reports a
  DCERST Max of 0.  This matches the primary event ring of the same model.
* The debug capability info context is checked but not used: the model
  does not enumerate, so the string descriptors are never handed to
  anyone.  Malformed ones - a non-zero length with a null pointer, a
  ``bLength`` that disagrees with the declared length, a
  ``bDescriptorType`` that is not ``STRING``, or a missing string 0 - are
  reported as guest errors.
* ``DCST`` reports a fixed debug port number once attached; the DbC does
  not actually consume one of the controller's root ports.
* The LTSSM beneath the port state machine is electrical and is not
  modelled at all; only its register-visible consequences, injected over
  the control chardev, are.
* The debug device is always reported as SuperSpeed, which is the only
  speed the DbC supports.

The DbC has no interrupt mechanism of its own - it has no interrupter
registers, and section 7.6.4.1 has software poll ``DCST.ER`` or the event
ring cycle bit - so the absence of interrupts here is the architecture, not
a modelling shortcut.
