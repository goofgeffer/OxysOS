# The Disk

**Phase**: 4, sub-task 4.4, of [`PLAN.md`](../project/PLAN.md).

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6. Every assertion of
hardware behaviour below carries a citation, and every specification named is
registered in [`REFERENCES.md`](../project/REFERENCES.md).

**Implementation**: [`../../drivers/ata/`](../../drivers/ata/), which holds six
translation units and the private header between them:
[`ata.c`](../../drivers/ata/ata.c) (the state, the refusals, the initialisation
and the block-layer binding),
[`port.c`](../../drivers/ata/port.c) (the task file and its timing rules),
[`identify.c`](../../drivers/ata/identify.c) (Section 3),
[`channel.c`](../../drivers/ata/channel.c) (Sections 2.1 to 2.3, where a channel
answers and what this driver cannot reach),
[`transfer.c`](../../drivers/ata/transfer.c) (Sections 4 and 5),
[`report.c`](../../drivers/ata/report.c) (Sections 7.2 and 7.3), and
[`internal.h`](../../drivers/ata/internal.h). It was one file of 1,282 lines
until the review that followed sub-task 6.10; see
[`../design/ARCHITECTURE.md`](../design/ARCHITECTURE.md), Section 2.2.
The public interface is
[`../../kernel/include/oxys/ata.h`](../../kernel/include/oxys/ata.h).

## 1. What is different about a disk

Every device driven before this one fails loudly. A keyboard that decodes a
scancode wrongly produces the wrong letter; a serial adapter at the wrong rate
produces nothing legible; a display driver that misplaces the cursor produces a
screen a person can see is wrong.

A disk driver that reads the wrong sector returns data. Data that arrived is
indistinguishable from data that is correct until something tries to interpret
it, and by then the kernel has built a filesystem upon it. An address composed
with one byte in the wrong register, a transfer of 255 words where 256 were due,
a second sector written over the first — each produces a disk that appears to
work and a filesystem that decays. Section 7 is longer than the corresponding
section of any other document in this project for that reason.

The second difference is that this is the first driver that can destroy
something. Every other device is an interface: a mistake costs an unreadable
line. A mistaken write costs data that was there before the kernel booted.
Section 6 states what follows from that.

## 2. The registers

The device presents two blocks of registers. The command block holds the task
file — the address, the count, the command and the status — and the control
block holds the alternate status and device control register.

| Offset from base | Read | Written |
| ---------------- | ---- | ------- |
| 0 | Data | Data |
| 1 | Error | Features |
| 2 | Sector count | Sector count |
| 3 | LBA low | LBA low |
| 4 | LBA mid | LBA mid |
| 5 | LBA high | LBA high |
| 6 | Device | Device |
| 7 | Status | Command |

In the compatibility addressing inherited from the IBM Personal Computer AT, the
first channel's command block is at `0x01F0` and its control block at `0x03F6`;
the second channel answers at `0x0170` and `0x0376`. Those are the addresses this
driver uses. An IDE controller found upon the PCI bus reports in bits 0 and 2 of
its programming interface whether each channel is in that compatibility mode or
in the native mode that takes its addresses from the base address registers; see
Section 2.1.

The bits this driver reads and writes:

| Register | Bit | Meaning |
| -------- | --- | ------- |
| Status | 7 (BSY) | The device owns the command block. Nothing else in the register is meaningful while it is set. |
| Status | 6 (DRDY) | The device is ready to accept a command. |
| Status | 5 (DF) | A device fault. It does not set ERR, so a driver that tested only ERR would proceed. |
| Status | 3 (DRQ) | A block of data is ready to be transferred. |
| Status | 0 (ERR) | The command failed; the error register describes it. |
| Device control | 1 (nIEN) | Set: the device does not assert its interrupt. |
| Device control | 2 (SRST) | Set and then cleared: both devices upon the channel are reset. |

### 2.1 Where a channel answers, which is not always where the AT put it

The addresses `0x01F0`/`0x03F6` and `0x0170`/`0x0376` are the **compatibility**
addresses, inherited from the IBM Personal Computer AT. They are where a channel
answers only while it is in compatibility mode.

A PCI IDE controller states, in the low bits of its programming interface byte,
which of its two channels are in **native PCI mode** instead:

| Bit | Meaning when set |
| --- | ---------------- |
| 0 | The primary channel is in native mode |
| 1 | The primary channel's mode may be changed |
| 2 | The secondary channel is in native mode |
| 3 | The secondary channel's mode may be changed |

A native channel answers at the addresses its base address registers give and at
no others — BAR0 and BAR1 for the primary, BAR2 and BAR3 for the secondary. So a
driver that probes the compatibility addresses regardless finds **nothing**, and
the machine appears to have no disk.

Two details of that mapping are easy to get wrong and both are asserted:

- **The control block register is at offset 2 within the four bytes the control
  BAR describes**, not at its start. A driver taking the base itself writes the
  device control register to a reserved port, so the software reset does nothing
  and the device's interrupt is never disabled — and the channel then appears to
  work until something raises IRQ14 that nothing has claimed.
- **The secondary channel reads the third and fourth registers.** Taking the
  first pair for both channels would put both channels' commands to the
  primary's ports.

A controller declaring native mode with no address assigned, or with a register
describing memory rather than ports, is disregarded and the channel left at its
compatibility address. Following either would put ATA commands to an arbitrary
port, and an arbitrary port belongs to some other device: the failure would not
be a missing disk but whatever that device does when written to.

### 2.2 What this driver cannot reach, and why that looked like a broken driver

This is a driver for the ATA command block registers, reached through I/O ports.
It therefore drives an **IDE controller**, in either mode, and nothing else.

That is not a small omission upon a modern machine. A firmware that presents its
SATA controller in **AHCI** mode — a serial ATA controller, class `0x01`,
subclass `0x06`, programming interface `0x01` — puts the disks behind registers
that are memory-mapped, and the controller answers at no I/O port whatever. An
**NVM Express** controller is not an ATA device at all.

The symptom of either is exactly the symptom of having no disk, and until this
was reported from a real machine the kernel said only:

```
ATA: no device answered upon either channel.
```

That one line had two quite different causes behind it, and no way to tell them
apart. It now says which:

```
ATA: primary channel at 0x1F0, control 0x3F6, the compatibility address.
ATA: secondary channel at 0x170, control 0x376, the compatibility address.
ATA: no device answered upon either channel.
ATA:   serial ATA controller, interface 0x1: an AHCI controller. Its registers
       are memory-mapped and it answers at no I/O port, so it is driven by the
       AHCI driver and not by this one.
ATA: this driver reads and writes through the ATA command block registers,
ATA: which is an IDE controller and nothing else. An AHCI controller named
ATA: above is reached by the AHCI driver instead, whose report follows this
ATA: one and says what it found upon each of its ports.
```

Until sub-task 4.7 the last four lines named a **remedy** rather than a driver:
where the firmware offered a storage mode of IDE, Legacy or Compatibility in
place of AHCI, selecting it made the disks visible to this kernel. That was the
best that could honestly be said of a kernel which could not reach them, and it
was addressed to whoever was standing at the machine.

It is no longer the best that can be said. Sub-task 4.7 drives the controller,
so this driver's business with it is to name it and stand aside; see
[`AHCI.md`](AHCI.md). Leaving the old advice in place would have sent a person
into a firmware menu to work around a driver the kernel now has.

### 2.3 The storage that is not of the storage class

Naming the AHCI controller was still not enough, because the machine the fault
was first reported from does not have one.

It is an **HP Laptop 14-dq0052dx**: an Intel Celeron N4120, four gibibytes of
memory, and its system upon a 64 GB **embedded MultiMediaCard** part rather than
a disk. It was booted from a **USB drive**. The full specification is in
[`../project/TESTING.md`](../project/TESTING.md), Section 5.1, and is not
restated here. Neither the eMMC part nor the USB drive is of the mass-storage
class. An eMMC part
is attached to an SD host controller, which the assignment specification classes
as a *system peripheral* — class `0x08`, subclass `0x05`. A USB drive is attached
to a serial-bus controller — class `0x0C`, subclass `0x03`.

Such a machine carries **no mass-storage controller at all**. The report searched
that class, found it empty, and said:

```
ATA:   the bus carries no mass-storage controller; this machine has no disk.
```

which was false, and false in the worst direction. The machine has two kinds of
storage and had just booted from one of them. Telling its owner that it has no
disk sends them to look for a fault in hardware that has none, or into a firmware
menu for a setting that does not exist there. The AHCI advice printed above it
would have been worse still: there is no AHCI controller to switch, and no
storage mode to choose.

The report now walks the whole recorded table and names the storage that is not
of the storage class:

```
ATA: primary channel at 0x1F0, control 0x3F6, the compatibility address.
ATA: secondary channel at 0x170, control 0x376, the compatibility address.
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

The last three lines said, until sub-task 4.8, that reaching the storage above
needed a driver this kernel did not have. It has one now — see
[`SDCARD.md`](SDCARD.md) — and the message says which report to read instead of
naming an absence.

The two paragraphs are alternatives, and which is printed turns upon whether
anything of the mass-storage class was found. Where there is such a controller
the remedy may be a firmware setting and is named; where there is not, no setting
will produce one and saying so plainly is the whole of what can honestly be
offered. The classification is `AtaClassifyForeignStorage` in `drivers/ata/channel.c`,
asserted at Section 7.3.

A subclass is read only against its own class, never alone. Subclass `0x05` is an
SD host controller under the system-peripheral class, an ATA controller under the
mass-storage class, and an **SMBus** controller under the serial-bus class. A
classifier that read the subclass by itself would offer a machine's SMBus as a
place its disks might be.

## 3. The 400 nanoseconds

After a command is written, and after a device is selected, the status register
does not describe the new state for 400 nanoseconds. A driver that read it
immediately would read the state before the command and conclude that a device
which is about to become busy is idle.

The delay is obtained by reading the **alternate** status register several times.
Two things make that the right instrument:

- It is the same value as the status register with no side effect. Reading the
  status register itself acknowledges a pending interrupt, which this driver has
  no business doing.
- An input from an I/O port may be assumed to take at least 30 nanoseconds, so
  fourteen reads before the one that is believed give better than 400. The driver
  performs fifteen.

That is also the only clock this driver has. The interval timer counts by
interrupt and the interrupt flag is clear throughout initialisation, so a wait
expressed in milliseconds is not available. Every delay and every timeout here is
counted in port reads for that reason, and the timeout is set high enough that no
healthy device reaches it and low enough that an absent one does not stop the
machine.

## 4. Identification

`IDENTIFY DEVICE` (ECh) is the question "what are you", and the interesting part
of it is the several ways it can be answered.

1. **Nothing is there.** The bus floats, and reads as all ones; a status of zero
   likewise means no device. Neither is an error.
2. **An ATA device answers.** BSY clears, DRQ sets, and 256 words of
   identification data follow. Words 60 and 61 hold the number of sectors 28-bit
   addressing can name; bit 10 of word 83 declares support for the 48-bit
   commands, and words 100 to 103 then hold the larger count. Words 27 to 46 are
   the model and 10 to 19 the serial number, each word holding two characters
   with the first in its **high** half — the opposite of the order the processor
   would store them in, which is why the extraction swaps them.
3. **A packet device declines.** An optical drive sets ERR and leaves the
   signature 14h, EBh in the LBA mid and high registers. A serial ATA device
   behind a compatibility bridge leaves 3Ch, C3h. An ATA device that aborted the
   command for some other reason leaves zeroes. Reading that signature is the
   only way to tell the three apart, and without it a packet device would be
   recorded as a broken disk.

A machine that booted from an optical medium has an ATAPI device and no disk.
That is not an error, and `AtaFirstDisk` returns nothing rather than offering the
optical drive to a caller that means to read a filesystem.

## 5. Reading and writing

A command is composed by writing the count and the address into the task file and
then the opcode into the command register. Three points are not obvious.

**The count of zero means the maximum.** The register holds the sector count in
one byte for the 28-bit commands and two for the 48-bit ones, and a register
value of zero means the greatest count the mode allows. The driver's limits are
therefore 256 and 65536, not 255 and 65535, and a request larger than one command
may carry is divided into several.

**The 28-bit address is partly in the device register.** Its four most
significant bits share the register that selects master or slave, so that
register is part of the address and not only a selection — which is why the
driver's cached selection is composed from both.

**The 48-bit form writes each register twice.** The high-order byte is written
first and the low-order byte second, the device retaining the previous content of
each register in a hidden half. That is the whole of the mechanism by which six
address bytes and two count bytes pass through four registers.

The mode is chosen for the request: 48-bit where the device supports it and the
request reaches beyond what 28 bits can name or asks for more than 256 sectors,
28-bit otherwise. The 28-bit commands are preferred where they suffice because
every device understands them, including one whose declaration of 48-bit support
is mistaken.

Between sectors the device is given its 400 nanoseconds to withdraw DRQ. Without
that pause the status of the sector just transferred would be read as though it
described the next one, and the driver would transfer a sector that had not
arrived.

**The receiving side uses a string instruction and the transmitting side does
not.** `REP INSW` moves the 256 words of a sector without the overhead of 256
separate transfers, upon a path that is already the slowest way to reach a disk.
There is no corresponding `REP OUTSW`: a device is entitled to a short recovery
between the words it is given, which the string form does not allow for and which
some devices are documented to require, so the transmitting side is a loop.

**A write is not finished when the data has been accepted.** The cache is flushed
by `FLUSH CACHE` (E7h), or `FLUSH CACHE EXT` (EAh) for a 48-bit command, within
the same sequence. A device that has accepted the data and not committed it
reports success, and the loss appears only upon a later read — which is to say,
as corruption with no failure attached to it.

## 6. The driver does not write unless it is told to

The self-test reads unconditionally. It writes only when the operator has asked
for it upon the kernel command line, by booting the GRUB entry *Oxys-OS (disk
write self-test)*, which passes the option `disk-write-test`.

This is not caution for its own sake. Anybody may boot this kernel upon their own
machine from the ISO, and the first disk of that machine holds their data. A
boot-time self-test that wrote to it unbidden would destroy that data, and the
project's own testing mandate — that every property be asserted at each boot —
does not extend to destroying the machine the assertion is made upon.

When the option is given, the test still refuses to be destructive: it reads the
sector it means to write, writes the pattern, reads it back, compares, and then
restores the sector from what it first read, verifying the restoration in its
turn. A test that damaged the disk and reported success would be worse than no
test at all.

The option is matched as a complete word rather than as a substring. An option is
a decision the operator made, and a decision must not be triggered by a longer
word that happens to contain it.

## 7. Verification

### 7.1 What the self-test asserts

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| The identification yielded a capacity and a model. | Identification data read but not understood: a byte order taken backwards, or a word index off by one. |
| The first sector reads, and reads the same way twice. | A transfer that leaves stale content in the buffer, or a device left in a state the next command inherits. |
| A two-sector read begins with what a one-sector read returned. | The count register mishandled, so that the second sector overwrites the first. |
| The second sector of a two-sector read is what a read of the following address returns. | Loss of synchronisation between sectors — a missing inter-sector wait, or a transfer of the wrong number of words, which shifts every sector after the first. |
| A range beyond the capacity is refused. | A read that wraps, or that the device answers with something. |
| A request with no buffer, and one for no sectors, are handled without harm. | A null dereference in the path that will one day be reached from a filesystem. |
| A sector beyond the 28-bit limit reads, where the device is large enough. | The 48-bit path never exercised at all. Its register discipline differs in kind and not only in width, so a driver that has never issued one has not been tested in that mode. |
| No device exceeded the driver's patience. | A timeout treated as an empty read. |
| With the option given: a pattern written to a sector reads back byte for byte, and the sector is then restored to what it held before. | Everything above, from the writing side; and any failure of the flush, which would otherwise appear as a later read returning the old contents. |

### 7.2 The addressing, asserted upon headers no machine here has

The decision of Section 2.1 is a pure function of a PCI configuration header, and
is exposed as one for exactly that reason: **no board available to this project
presents an IDE controller in native mode.** Every one of them uses the
compatibility addresses, so there is nothing here to probe, and a machine that
did could not be obtained to try it upon.

Headers are therefore composed and the decision asked about them. The alternative
was to write the arithmetic and hope — which is what produced the fault being
corrected, and which is discovered to be wrong by somebody else, upon their own
machine, with no diagnostic beyond a disk that is not there.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| A channel in compatibility mode is not moved, whatever its base address registers hold. | Reading the registers without consulting the programming interface, and then probing ports the controller does not decode upon a machine whose disks were at `0x1F0` all along. |
| A native channel takes its command base from its own register, and its control base from **two bytes into** the next. | Section 2.1: a reset that does nothing and an interrupt never disabled. |
| The secondary channel reads the third and fourth registers. | Both channels addressed at the primary's ports. |
| One channel native and the other not is honoured for each separately. | A single test applied to both. |
| A native declaration with no address assigned is refused. | ATA commands issued to port zero. |
| A register describing memory is not read as I/O ports. | The same, to an arbitrary port belonging to another device. |
| The programming interface is read only for subclass `0x01`. | An **AHCI** controller reports interface `0x01` — the same bit that marks an IDE primary channel as native — so a driver that skipped the subclass check would read its memory registers as I/O ports. This is not hypothetical: `0x01` is what the controller in this project's own QEMU board reports. |

### 7.3 The storage that is not of the storage class, asserted the same way

The classification of Section 2.3 is a pure function of a configuration header
for the same reason as the addressing: **no board available to this project
presents an SD host controller** in the ordinary course. Headers are composed and
the classification asked about them.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| An SD host controller is recognised as storage. | The report telling the owner of a laptop whose system is upon an eMMC part that the machine has no disk. |
| A USB controller is recognised as storage. | The same, for the drive the kernel was booted from. |
| A subclass is read only against its own class. | Subclass `0x05` is an SD host controller, an ATA controller or an **SMBus** controller according to its class; subclass `0x03` is a USB controller or something else entirely. Reading either alone offers a machine's SMBus as a place its disks might be. |
| An IDE or AHCI controller is **not** counted here. | Counting one would print it twice, and would print the firmware remedy — which turns upon whether anything of the mass-storage class was found — for a machine that has no such controller and therefore no such remedy. |
| A function that names nothing is not storage. | A null dereference in the path that runs only upon a machine that has already failed to find a disk. |

The classification was then observed upon a board composed to have the shape of
the machine that reported the fault: no mass-storage controller at all, an SD
host controller, and the kernel booted from a USB drive.

```sh
qemu-system-x86_64 -machine q35,sata=off -cpu qemu64 -smp cores=2 -m 512M \
    -device sdhci-pci -device qemu-xhci,id=xhci \
    -drive if=none,id=usbstick,file=build/oxys.iso,format=raw,media=cdrom \
    -device usb-storage,bus=xhci.0,drive=usbstick \
    -display none -serial stdio
```

`sata=off` removes the board's own AHCI controller, which is what leaves the
mass-storage class empty. Observed:

```
  0:3.0  0x1B36:0x7  SD host controller (class 0x8, subclass 0x5, interface 0x1), IRQ 11
  0:4.0  0x1B36:0xD  USB controller (class 0xC, subclass 0x3, interface 0x30), IRQ 10
...
ATA: no device answered upon either channel.
ATA:   SD host controller, interface 0x1: where an embedded MultiMediaCard or a
       card in a slot is attached. It is not an ATA device and has no command
       block registers.
ATA:   USB controller, interface 0x30: where a USB drive is attached. It is not
       an ATA device and has no command block registers.
ATA: this machine has no mass-storage controller at all, so there is no
ATA: firmware setting that would present its storage as a disk. Reaching
ATA: the storage above needs a driver this kernel does not yet have.
```

### 7.4 What the self-test cannot assert

It cannot assert that a sector holds what the operator put there — the kernel has
no independent knowledge of the medium. That is established from outside, and was
established for this sub-task: a raw image was seeded at three addresses, and the
driver read each of them back exactly.

```sh
qemu-img create -f raw disk.img 256G
# sector 0 seeded 'OXYS-SECTOR-ZERO', sector 1 'OXYS-SECTOR-ONE',
# and sector 0x10000001 — beyond what 28 bits can name — 'OXYS-BEYOND-28-BITS'.

qemu-system-x86_64 -machine pc -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso \
    -drive file=disk.img,format=raw,if=ide,index=0,media=disk \
    -display none -serial file:ata.log
```

The image is 256 GiB and sparse: it occupies a few kilobytes upon the host and
still presents 536870912 sectors, which is twice what 28-bit addressing can name.
That is what makes the 48-bit path reachable at all.

Observed:

```
Disk self-test passed.
ATA: 2 devices, polled, device interrupts disabled.
  primary master: ATA disk, 536870912 sectors (268435456 KiB), 48-bit addressing, QEMU HARDDISK
  secondary master: ATAPI packet device
ATA: commands 9, sectors read 6, written 0, device errors 0, requests refused 3,
     timeouts 0, last error: no device or no buffer.
```

The optical drive upon the second channel is the ISO the kernel booted from,
recognised by its signature rather than mistaken for a disk.

The three refusals are the ones the self-test provoked: two ranges beyond the
capacity and one request without a buffer. A refusal is counted apart from an
error of the hardware because the two mean opposite things — a refusal is the
driver working, the caller having asked for something impossible and been told so
before the disk was touched — and a figure that added them together would show a
healthy machine accumulating errors until an operator learned to ignore the
number.

### 7.5 The machine used for the disk tests

`make verify` runs upon the q35 board, whose storage controller is AHCI and which
presents no device this driver can address; the disk self-test there reports that
nothing answered and asserts nothing, which is the correct outcome and is
recorded as such. The disk tests are run upon the i440fx board, which presents
the PIIX3 IDE controller at `0:1.1` in compatibility mode. Both are recorded in
[`TESTING.md`](../project/TESTING.md).

## 8. Limitations

1. **IDE controllers only.** The driver reaches an IDE controller in either
   mode — compatibility or native, since Section 2.1 — and nothing else. An AHCI
   controller and an NVM Express controller are both invisible to it. That is no
   longer the same as having no disk: since sub-task 4.7 the AHCI controller is
   driven by [`AHCI.md`](AHCI.md), and this driver's business with it is to name
   it and stand aside. An NVM Express controller is still reached by nothing. See
   Section 2.2.
2. **Storage that is not of the storage class is named, not reached.** An eMMC
   part behind an SD host controller and a USB drive behind a serial-bus
   controller are both storage and neither is an ATA device. This driver says
   where they are and can do nothing else. Since sub-task 4.8 the first of them
   is driven by [`SDCARD.md`](SDCARD.md); a USB drive is reached by nothing yet.
   See Section 2.3.
3. **A controller in native mode is followed, not switched.** Where the
   programming interface says a channel's mode may be changed, this driver does
   not change it: it reads the addresses the firmware assigned and uses those.
   Switching would be the smaller change and the worse one, since the
   compatibility ports may already belong to something else on a machine whose
   firmware chose otherwise.
4. **Polled, not interrupt-driven.** The device's interrupt is disabled at the
   device by nIEN, rather than merely masked, because nothing claims IRQ14 or
   IRQ15 and a request that nothing claims is counted as unclaimed upon every
   command. A transfer therefore occupies the processor entirely.
5. **No direct memory access.** Programmed input/output moves every word through
   a register. Bus mastering is what makes a disk fast and it belongs with the
   block layer of sub-task 4.5.
6. **No ATAPI commands.** A packet device is recognised and then left alone.
   Reading from one requires the packet interface, which is a command set of its
   own.
7. **No concurrency safety.** The driver has no lock, and the command block of a
   channel is a single resource shared by its two devices. Nothing else in the
   kernel touches a disk yet.
8. **No retry.** A command that fails is reported, not repeated. What to retry
   and how often is a policy, and the block layer is where a policy belongs.
