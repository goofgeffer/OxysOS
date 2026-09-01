# The Disk

**Phase**: 4, sub-task 4.4, of [`PLAN.md`](../project/PLAN.md).

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6. Every assertion of
hardware behaviour below carries a citation, and every specification named is
registered in [`REFERENCES.md`](../project/REFERENCES.md).

**Implementation**: [`../drivers/ata/ata.c`](../../drivers/ata/ata.c),
[`../kernel/include/oxys/ata.h`](../../kernel/include/oxys/ata.h).

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
Section 8.

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

### 7.2 What the self-test cannot assert

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

### 7.3 The machine used for the disk tests

`make verify` runs upon the q35 board, whose storage controller is AHCI and which
presents no device this driver can address; the disk self-test there reports that
nothing answered and asserts nothing, which is the correct outcome and is
recorded as such. The disk tests are run upon the i440fx board, which presents
the PIIX3 IDE controller at `0:1.1` in compatibility mode. Both are recorded in
[`TESTING.md`](../project/TESTING.md).

## 8. Limitations

1. **Compatibility addressing only.** The driver uses `0x01F0` and `0x0170`. It
   does not read the base address registers of a controller in native mode, nor
   set the bits of the programming interface that would return it to
   compatibility mode. Every machine of interest presents the legacy addresses;
   the PCI enumeration of [`PCI.md`](../devices/PCI.md) records the controller, and using
   what it recorded is the natural next step.
2. **Polled, not interrupt-driven.** The device's interrupt is disabled at the
   device by nIEN, rather than merely masked, because nothing claims IRQ14 or
   IRQ15 and a request that nothing claims is counted as unclaimed upon every
   command. A transfer therefore occupies the processor entirely.
3. **No direct memory access.** Programmed input/output moves every word through
   a register. Bus mastering is what makes a disk fast and it belongs with the
   block layer of sub-task 4.5.
4. **No ATAPI commands.** A packet device is recognised and then left alone.
   Reading from one requires the packet interface, which is a command set of its
   own.
5. **No concurrency safety.** The driver has no lock, and the command block of a
   channel is a single resource shared by its two devices. Nothing else in the
   kernel touches a disk yet.
6. **No retry.** A command that fails is reported, not repeated. What to retry
   and how often is a policy, and the block layer is where a policy belongs.
