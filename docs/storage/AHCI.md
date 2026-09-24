<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The AHCI Disk

**Phase**: sub-task 4.7 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../drivers/ahci/ahci.c`](../../drivers/ahci/ahci.c),
[`../../kernel/include/oxys/dev/storage/ahci.h`](../../kernel/include/oxys/dev/storage/ahci.h).
**Specifications**: Serial ATA AHCI Specification 1.3.1; ATA/ATAPI Command Set
(`READ DMA EXT`, `WRITE DMA EXT`, `FLUSH CACHE EXT`, `IDENTIFY DEVICE`); Intel
SDM, Volume 3A, Section 11.3.4. All in
[`../project/REFERENCES.md`](../project/REFERENCES.md).

The driver for a serial ATA controller in AHCI mode. Such a controller answers at
no I/O port, so the ATA driver of [`DISK.md`](DISK.md) sees no disk at all. The
two drivers share a command set and nothing else:

| | ATA ([`DISK.md`](DISK.md)) | AHCI |
| --- | --- | --- |
| The command | Written byte by byte to I/O ports | Composed in memory as a FIS |
| Who moves the data | The processor, one word per `IN` | The adaptor, by bus mastering |
| Where the data goes | Wherever the processor puts it | The caller's pages, named by physical address |
| Cost of a transfer | The processor, throughout | One command issue and one poll |

Every structure composed here is read by the device, so it is described by
physical address, and a caller's buffer must stay resident during the transfer.

## 1. Taking the adaptor

An AHCI controller is class `0x01`, subclass `0x06`, interface `0x01`; a SATA
controller with another interface has other registers.

1. **Enable memory space and bus mastering** in the PCI command register. Without
   the first the mapping decodes to nothing; without the second the adaptor
   accepts commands and never fetches them. Firmware that did not boot from the
   disk may have set neither.
2. **Map `ABAR`** (base address register 5) uncached. A cached device register
   returns the first value ever read from it.
3. **Ask the firmware to let go.** If `CAP2.BOH` shows the BIOS/OS handoff and
   `BOHC.BOS` shows the firmware owns the controller, set `BOHC.OOS` and wait for
   the firmware to clear its bit. System management code may be driving the
   controller for the firmware; reprogramming the ports under it leaves two
   owners.
4. **Set `GHC.AE`** before reading anything else: an adaptor not yet enabled
   presents its registers in a legacy arrangement.

## 2. Registers

| Register | Offset | Use |
| -------- | ------ | --- |
| `CAP` | `00h` | Slot count; `S64A`, whether addresses above 4 GiB work |
| `GHC` | `04h` | `AE` (bit 31) |
| `PI` | `0Ch` | Which ports exist: a bitmap, not a count |
| `VS` | `10h` | Reported only |
| `CAP2` | `24h` | `BOH` (bit 0) |
| `BOHC` | `28h` | The handoff |

Port *x* starts at `100h + x * 80h`:

| Register | Offset | Use |
| -------- | ------ | --- |
| `PxCLB`, `PxCLBU` | `00h`, `04h` | Command list physical address |
| `PxFB`, `PxFBU` | `08h`, `0Ch` | Received FIS area physical address |
| `PxIS`, `PxIE` | `10h`, `14h` | Status, polled; enable, left at zero |
| `PxCMD` | `18h` | `ST` bit 0, `FRE` bit 4, `FR` bit 14, `CR` bit 15 |
| `PxTFD` | `20h` | Device status in bits 7:0 |
| `PxSIG` | `24h` | What is attached |
| `PxSSTS` | `28h` | Link state |
| `PxSERR` | `30h` | Cleared by writing back its value |
| `PxCI` | `38h` | Command issue, one bit per slot |

**`PI` is a bitmap.** A board implementing ports 0 and 2 only is ordinary; a
driver that walked *n* consecutive ports would program a port that does not
exist and skip one that does.

## 3. Is anything attached?

`PxSSTS` fields, read together:

- `DET` (bits 3:0) must be 3: present, with communication established. 1 is a
  device still negotiating, which would never answer a command.
- `IPM` (bits 11:8) must be 1: active. 2 (partial) and 6 (slumber) are present
  but asleep, indistinguishable from ready if `DET` is read alone.
- The speed (bits 7:4) lies between them and belongs to neither; a mask that
  includes it rejects every link faster than the slowest.

`PxSIG` then names the device: `00000101h` disk, `EB140101h` packet device,
`C33C0101h` enclosure, `96690101h` port multiplier. All end in `0101h`, so
comparing the low half calls each a disk, and a packet device would be sent
`READ DMA EXT`.

## 4. Port memory

One page per active port:

| Offset | Structure | Alignment required |
| ------ | --------- | ------------------ |
| `0000h` | Command list: 32 headers of 32 bytes | 1024 |
| `0400h` | Received FIS area, 256 bytes | 256 |
| `0500h` | Command table of slot 0 | 128 |
| `0580h` | Its region descriptors | — |
| `0800h` | The `IDENTIFY DEVICE` buffer | — |

A frame is page-aligned, so each offset meets its alignment absolutely. The frame
is taken **below 4 GiB** when `CAP.S64A` is clear: an adaptor given a 64-bit
address it cannot express fetches the low half of it, another page of the kernel,
and executes it as a command list.

**No cache flushing.** The processor's caches stay coherent with bus-master
reads (Intel SDM, Volume 3A, Section 11.3.4). What matters is order: every
structure is written through `volatile` pointers and so is the `PxCI` write that
issues the command, so the compiler cannot issue the command before describing it.

## 5. A command

**Slot zero, one at a time**, polled to completion. Interrupts are masked during
initialisation and nothing can be woken, so queuing 32 commands would only mean
polling 32 slots.

**The command header's first double word** is three fields in one word, each
wrong silently, and is built by `AhciDescribeCommand`:

| Bits | Field | If wrong |
| ---- | ----- | -------- |
| 4:0 | FIS length in double words (5) | In bytes, 20 does not fit and becomes 4. |
| 6 | Write | The disk is transferred into the buffer meant to be written from, and success reported. |
| 31:16 | Region descriptor count | Zero transfers nothing and succeeds. |

**Region descriptors.** A buffer contiguous to the processor need not be to the
device, so each page becomes its own descriptor, translated through the page
tables when the command is composed. A descriptor's byte count is **one less**
than the bytes it covers. Its address must be even; a buffer at an odd address is
refused, since rounding would write the byte below.

**The FIS**, Register Host to Device (`27h`), 20 bytes:

| Byte | Content |
| ---- | ------- |
| 0 | `27h` |
| 1 | Bit 7: a command, not a register update |
| 2 | The command |
| 4–6 | Address bytes 0–2 |
| 7 | Device register; bit 6 for LBA |
| 8–10 | Address bytes 3–5 |
| 12–13 | Sector count |

Only extended commands are issued, so there is no 28-bit path. A byte placed in
the wrong half of the address reads a sector a multiple of 16 MiB away,
successfully.

**Completion.** `PxCI` bit 0 clears. `PxIS.TFES` (bit 30) marks a device error,
and `PxTFD` bit 0 is read too, for a device that fails without setting it. A
command that never completes exhausts `AHCI_WAIT_LIMIT` reads and is counted as
a **timeout**, apart from errors: the two mean opposite things.

**A write is `WRITE DMA EXT` then `FLUSH CACHE EXT`.** Data accepted into the
device's cache can still be lost, and the caller told it succeeded could never
find out.

## Verification

`KernelVerifyAhci` in [`../../kernel/test/storage/stack.c`](../../kernel/test/storage/stack.c).
No available board presents a packet device, enclosure or multiplier on AHCI, or
a sleeping port, so those decisions are asserted on the values hardware would
give.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| `DET` 3 with `IPM` 1 is usable; the speed field is ignored. | Every fast link rejected. |
| `DET` 0, 1 or 4 is not usable. | A command sent to a link that is not up, and the whole timeout spent. |
| `IPM` 0, 2 or 6 is not usable. | The same, for a sleeping interface. |
| The four signatures are distinguished, and an unknown one is no device. | A packet device driven as a disk. |
| The command header places length, write bit and region count correctly. | A FIS four times too long, a reversed transfer, or no transfer. |

Where a disk answers:

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| A sector read twice into differently seeded buffers agrees over the whole sector. | A short transfer; a consistently wrong length leaves identical wrong buffers unless they start different. |
| Nothing beyond the sector was written. | A byte count too large. |
| A two-sector read starts with the one-sector read and continues with the next address. | The count mishandled; sectors out of step. |
| A sector beyond the 28-bit limit reads. | The address split wrongly across the FIS. |
| Out-of-range, zero, over-limit, bufferless and odd-address requests are refused. | A transfer the adaptor should never see. |
| With the write option, a pattern written to the last sector reads back, and the sector is restored. | The write path and the flush. |

Not assertable here: a region byte count one too large (the length instead of the
length less one). The adaptor transfers what the command's sector count asks for
and never reaches the extra byte; an adaptor that validated the field would
reject it, and neither available adaptor does.

`make verify` runs on q35, whose AHCI controller carries the boot ISO as a packet
device on port 2, so taking the adaptor, the handoff, the port setup and the
signature run at every boot. To exercise transfers, attach a disk:

```sh
qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso \
    -drive file=disk.img,format=raw,if=none,id=sata0 \
    -device ide-hd,drive=sata0,bus=ide.0 \
    -display none -serial file:ahci.log
```

VirtualBox's ICH8-M AHCI controller is a second, independent design.

## Limitations

1. One command at a time, slot zero; no native command queuing, which needs a
   scheduler to wait on.
2. Polled; `PxIE` is zero and no handler is registered. A transfer occupies the
   processor.
3. At most 128 sectors (64 KiB, 17 descriptors) per command; more needs a larger
   command table than one page per port allows.
4. Packet devices are recognised and left alone.
5. Port multipliers are named and not followed.
6. Ports are enumerated once; no hot plug.
7. A failed command is reported, not retried, and the port is not restarted.
8. On a 32-bit adaptor, a buffer above 4 GiB is refused rather than bounced;
   nothing in the kernel allocates that high.
