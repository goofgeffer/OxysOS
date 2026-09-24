<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The ATA Disk

**Phase**: sub-task 4.4 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../drivers/ata/`](../../drivers/ata/) — `ata.c` (state,
refusals, initialisation, block-layer binding), `port.c` (the task file and its
timing), `identify.c` (Section 4), `channel.c` (Sections 2 and 3), `transfer.c`
(Section 5), `report.c` (Section 3); the interface
[`../../kernel/include/oxys/dev/storage/ata.h`](../../kernel/include/oxys/dev/storage/ata.h).
**Specifications**: ATA/ATAPI Command Set (ATA-8); PCI IDE Controller
Specification (the programming interface); PCI Code and ID Assignment
Specification. All in [`../project/REFERENCES.md`](../project/REFERENCES.md).

The driver for disks behind an IDE controller, through the ATA command block
registers in I/O space. Two properties set a disk apart from every earlier
device. **A wrong read returns data**: an address byte in the wrong register, a
transfer one word short, or a second sector over the first all produce a disk that
appears to work and a filesystem that decays, which is why the verification is
long. **A wrong write destroys what was there before the kernel booted**, which is
why Section 6 exists.

## 1. Registers

The command block holds the task file; the control block holds the alternate
status and device control registers.

| Offset | Read | Written |
| ------ | ---- | ------- |
| 0 | Data | Data |
| 1 | Error | Features |
| 2 | Sector count | Sector count |
| 3–5 | LBA low, mid, high | LBA low, mid, high |
| 6 | Device | Device |
| 7 | Status | Command |

| Register | Bit | Meaning |
| -------- | --- | ------- |
| Status | 7 BSY | The device owns the block; nothing else in the register is valid. |
| Status | 6 DRDY | Ready for a command. |
| Status | 5 DF | Device fault. It does not set ERR, so a driver testing ERR alone proceeds. |
| Status | 3 DRQ | A block of data is ready. |
| Status | 0 ERR | The command failed; see the error register. |
| Device control | 1 nIEN | The device does not assert its interrupt. |
| Device control | 2 SRST | Set then cleared: resets both devices of the channel. |

## 2. Where a channel answers

The compatibility addresses are `0x01F0`/`0x03F6` (primary) and `0x0170`/`0x0376`
(secondary). A PCI IDE controller (class `0x01`, subclass `0x01`) says in its
programming interface whether each channel is instead in **native mode**:

| Bit | Set means |
| --- | --------- |
| 0 | Primary channel native |
| 1 | Primary channel's mode can be changed |
| 2 | Secondary channel native |
| 3 | Secondary channel's mode can be changed |

A native channel answers only at its base address registers: BAR0 and BAR1 for
the primary, BAR2 and BAR3 for the secondary. A driver probing the compatibility
addresses finds nothing.

- **The control register is at offset 2** of the four bytes its BAR describes.
  Taking the base writes device control to a reserved port: the reset does
  nothing and the interrupt is never disabled.
- **The secondary channel reads the third and fourth BARs.**
- **A native declaration with no address, or with a BAR describing memory, is
  ignored** and the channel stays at its compatibility address. Following it
  would send ATA commands to some other device's port.
- **The programming interface is read only for subclass `0x01`.** An AHCI
  controller reports interface `0x01` too, the bit that means "primary native",
  and QEMU's q35 controller is one.
- **A native channel is followed, not switched.** The compatibility ports may
  belong to something else on a machine whose firmware chose native mode.

## 3. What the driver cannot reach, and what it says

The driver reaches an IDE controller and nothing else. An AHCI controller
([`AHCI.md`](AHCI.md)), an SD host controller ([`SDCARD.md`](SDCARD.md)), a USB
drive and NVM Express all answer at no ATA port, and each looks exactly like
having no disk. When no device answers, the driver therefore names the storage it
can see and says which report to read, so a person is not sent looking for a
hardware fault or a firmware setting that does not exist.

With an AHCI controller present:

```
ATA: no device answered upon either channel.
ATA:   serial ATA controller, interface 0x1: an AHCI controller. Its registers
       are memory-mapped and it answers at no I/O port, so it is driven by the
       AHCI driver and not by this one.
ATA: this driver reads and writes through the ATA command block registers,
ATA: which is an IDE controller and nothing else. An AHCI controller named
ATA: above is reached by the AHCI driver instead, whose report follows this
ATA: one and says what it found upon each of its ports.
```

With no mass-storage controller at all (the eMMC laptop of
[`../project/TESTING.md`](../project/TESTING.md), booted from USB):

```
ATA: no device answered upon either channel.
ATA:   SD host controller, interface 0x1: where an embedded MultiMediaCard or a
       card in a slot is attached. It is not an ATA device and has no command
       block registers.
ATA:   USB controller, interface 0x30: where a USB drive is attached. It is not
       an ATA device and has no command block registers.
ATA: this machine has no mass-storage controller at all, so there is no
ATA: firmware setting that would present its storage as a disk. What it
ATA: does have is named above, and the report of whichever driver reaches
ATA: it follows this one. A USB drive is reached by nothing yet.
```

The classification is `AtaClassifyForeignStorage` in `channel.c`. **A subclass is
read only with its class**: `0x05` is an SD host controller under class `0x08`,
an ATA controller under `0x01`, and an SMBus controller under `0x0C`. Reading it
alone offers the SMBus as a place the disks might be.

## 4. Timing

After a command or a device selection the status register does not reflect the
new state for 400 ns. Reading it at once shows a device about to become busy as
idle.

The delay is fifteen reads of the **alternate** status register. It has no side
effect (reading status acknowledges an interrupt), and a port input takes at
least 30 ns, so fourteen reads exceed 400 ns. It is the only clock the driver
has: interrupts are masked during initialisation, so every delay and timeout is
counted in port reads, with the timeout high enough for any healthy device and
low enough that an absent one does not stop the machine.

## 5. Identification

`IDENTIFY DEVICE` (`ECh`) is answered in one of three ways:

1. **Nothing is there**: the bus floats and reads all ones, or status reads zero.
   Not an error.
2. **An ATA device**: BSY clears, DRQ sets, 256 words follow. Words 60–61 hold
   the 28-bit sector count; word 83 bit 10 declares 48-bit support, and words
   100–103 then hold the larger count. Words 27–46 are the model and 10–19 the
   serial, each word holding two characters with the first in the **high** byte,
   so the extraction swaps them.
3. **A refusal with a signature** in LBA mid and high: `14h EBh` a packet device
   (an optical drive), `3Ch C3h` a SATA device behind a compatibility bridge,
   zeroes an ATA device that aborted. Without reading it a packet device is
   recorded as a broken disk.

A machine booted from optical media has a packet device and no disk;
`AtaFirstDisk` then returns nothing rather than offer the drive to a filesystem.

## 6. Transfers

- **A count of zero means the maximum**: 256 sectors for 28-bit commands, 65,536
  for 48-bit. A larger request is split.
- **The 28-bit address's top four bits share the device register** with the
  master/slave selection, so the cached selection is composed from both.
- **48-bit commands write each register twice**, high byte first; the device
  keeps the earlier value in a hidden half. That is how six address bytes and two
  count bytes pass through four registers.
- **28-bit is preferred** whenever the request fits (below 2^28 and at most 256
  sectors), since every device understands it, including one wrongly declaring
  48-bit support.
- **400 ns between sectors**, for the device to withdraw DRQ; otherwise the last
  sector's status is read as the next one's.
- **`REP INSW` reads; a loop writes.** A device may need a recovery time between
  words it receives, which `REP OUTSW` does not allow.
- **A write ends with a flush**: `FLUSH CACHE` (`E7h`), or `FLUSH CACHE EXT`
  (`EAh`) after a 48-bit command. A device that accepted data but did not commit
  it reports success, and the loss surfaces later as silent corruption.
- **A refusal is counted apart from a device error.** A refusal is the driver
  working; a combined count shows a healthy machine accumulating errors.

## 7. Writing only when told

The self-test reads unconditionally and writes only when the kernel command line
contains the option `disk-write-test`, given by the GRUB entry *Oxys-OS (disk
write self-test)*. Anyone may boot this image on their own machine, whose first
disk holds their data.

With the option, the test reads the sector it will write, writes a pattern, reads
it back, compares, restores the original and verifies the restoration. The option
is matched as a whole word, so a longer word containing it does not trigger it.

## Verification

`KernelVerifyAta` in [`../../kernel/test/storage/stack.c`](../../kernel/test/storage/stack.c).

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| Identification yields a capacity and a model. | Identification data misread: byte order, or a word index off by one. |
| The first sector reads the same twice. | Stale buffer content, or a device state the next command inherits. |
| A two-sector read starts with the one-sector read, and continues with the next address. | The count mishandled; a missing inter-sector wait shifting every later sector. |
| A range beyond the capacity is refused. | A read that wraps. |
| A bufferless or zero-sector request does no harm. | A null dereference on a filesystem's path. |
| A sector beyond the 28-bit limit reads, where the disk is large enough. | The 48-bit path never exercised. |
| No device exceeded the timeout. | A timeout treated as an empty read. |
| With `disk-write-test`: a pattern reads back and the sector is restored. | The write path and the flush. |

The addressing and the classification are pure functions of a PCI header, and no
available board has a native-mode IDE controller or (ordinarily) an SD host
controller, so headers are composed and the functions asked:

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| A compatibility channel stays put, whatever its BARs hold. | Probing ports the controller does not decode. |
| A native channel takes its command base from its BAR and its control base two bytes into the next. | A reset that does nothing; an interrupt never disabled. |
| The secondary reads the third and fourth BARs; each channel's mode is honoured separately. | Both channels on the primary's ports; one test applied to both. |
| A native declaration without an address, or with a memory BAR, is refused. | Commands to port zero or another device's port. |
| The interface is read only for subclass `0x01`. | An AHCI controller's memory registers read as ports. |
| SD host and USB controllers are recognised as storage; a subclass is read only with its class. | A laptop told it has no disk; an SMBus offered as storage. |
| IDE and AHCI controllers are not counted as foreign storage. | A controller named twice, with the wrong remedy. |
| A function naming nothing is not storage. | A null dereference on the no-disk path. |

**From outside**, the self-test cannot know what a sector should hold. A sparse
256 GiB raw image seeded at sectors 0, 1 and `0x10000001` (beyond 28 bits) is read
back exactly:

```sh
qemu-img create -f raw disk.img 256G
qemu-system-x86_64 -machine pc -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
    -display none -serial file:ata.log
```

```
Disk self-test passed.
ATA: 2 devices, polled, device interrupts disabled.
  primary master: ATA disk, 536870912 sectors (268435456 KiB), 48-bit addressing, QEMU HARDDISK
  secondary master: ATAPI packet device
ATA: commands 9, sectors read 6, written 0, device errors 0, requests refused 3,
     timeouts 0, last error: no device or no buffer.
```

The packet device is the boot ISO; the three refusals are the test's own. The
i440fx board (`-machine pc`) is used because it has a PIIX3 IDE controller at
`0:1.1`; on q35, which `make verify` uses, the controller is AHCI and this test
correctly reports that nothing answered. To reproduce the no-disk report of
Section 3, boot q35 with `sata=off`, `-device sdhci-pci`, and the ISO on a
`usb-storage` device behind `qemu-xhci`.

## Limitations

1. IDE controllers only. AHCI and SD are other drivers; NVM Express and USB
   storage are reached by nothing.
2. A native channel is not switched to compatibility mode.
3. Polled. The interrupt is disabled at the device with nIEN, since nothing
   claims IRQ14 or IRQ15; a transfer occupies the processor.
4. Programmed I/O; no bus mastering.
5. Packet devices are recognised and left alone.
6. No lock; a channel's command block is shared by its two devices.
7. A failed command is reported, not retried.
