# QEMU with an xHCI Debug Capability (DbC) model

Prebuilt QEMU 10.2.4 with the xHCI **Debug Capability** (extended capability
ID 0x0a) modelled, so that a DbC stack can be exercised in a VM instead of
meeting real silicon blind.

The DbC is the two-endpoint bulk USB debug device an xHCI controller can
present on a root port.  This build implements the register block, the DbC
event ring, the debug capability context and the two bulk transfer rings,
and bridges the bulk pipes to a chardev — the chardev is the machine at the
far end of the debug cable.

Nothing here needs the QEMU source tree.  Everything the binary needs is in
the bundle.

## Running it

Linux:

```sh
tar xf qemu-dbc-*-linux-x86_64.tar.xz
cd qemu-dbc-*-linux-x86_64
./bin/qemu-system-x86_64 --version
```

Windows (from a command prompt in the unpacked folder):

```
qemu-system-x86_64.exe --version
```

The launcher on Linux is a small shell wrapper that points the loader at the
bundled `lib/` directory; the real binary is `bin/qemu-system-x86_64.bin`.
Keep `bin/`, `lib/` and `share/` together — QEMU locates its firmware blobs
relative to the executable.

## Using the Debug Capability

Attach a chardev to the xHCI controller with `dbc-chardev`:

```sh
./bin/qemu-system-x86_64 \
    -chardev socket,id=dbc0,path=/tmp/dbc.sock,server=on,wait=off \
    -device qemu-xhci,id=xhci,dbc-chardev=dbc0 \
    ...
```

Bytes the guest sends on the DbC bulk IN endpoint come out of the chardev;
bytes written to the chardev are delivered to the guest through the bulk OUT
endpoint.  The chardev peer being connected is what the guest sees as the
cable being plugged in: `DCPORTSC.CCS` asserts, and `DCCTRL.DCR` follows,
only while a peer is attached.  Disconnecting drops the link and posts a
port status change event.

`dbc=on` advertises the capability with no backend, which is only useful for
testing the discovery path.  Without either property the capability is not
advertised at all and the controller behaves exactly as it did before.

On Windows the same applies with a named pipe or TCP chardev, e.g.
`-chardev socket,id=dbc0,host=127.0.0.1,port=4444,server=on,wait=off`.

## What this does and does not validate

It validates the **software pipeline**: register programming, the TRB rings,
event handling and the byte transport.

It does **not** reproduce deep-sleep survival (Intel erratum PTL025), port
mapping, or any electrical behaviour, and it is not a substitute for
bring-up on real silicon.

Further limitations are listed in `docs/system/devices/xhci-dbc.rst` in the
source branch.

## Provenance

* Upstream base: QEMU `v10.2.4` (`stable-10.2`, commit `3e0bcba`).
* Source branch: `claude/qemu-xhci-dbc-model-0xrnue` in this repository —
  five commits on top of that tag.
* QEMU is licensed under the GPL v2; the DbC model files are
  LGPL-2.1-or-later, matching the xHCI model they are part of.  Full source
  for everything in these bundles is in this repository.
