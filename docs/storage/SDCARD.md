# The SD Card and the Embedded MultiMediaCard

**Phase**: 4, sub-task 4.8, of [`PLAN.md`](../project/PLAN.md).

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6. Every assertion of
hardware behaviour below carries a citation, and every specification named is
registered in [`REFERENCES.md`](../project/REFERENCES.md).

**Implementation**: [`../../drivers/sdhci/sdhci.c`](../../drivers/sdhci/sdhci.c),
[`../../kernel/include/oxys/sdhci.h`](../../kernel/include/oxys/sdhci.h).

## 1. Why this driver exists

An inexpensive laptop has no disk in any sense the other two storage drivers
understand. Its system sits upon an **embedded MultiMediaCard** part, which is
attached to a host controller the PCI assignment specification classes as a
*system peripheral* — class `0x08`, subclass `0x05` — and not as mass storage.
Such a machine carries **no mass-storage controller at all**.

That is not a corner case. It is the machine this whole line of work was reported
from: an Intel Celeron, four gibibytes of memory, eMMC storage, booted from a USB
drive. Neither the ATA driver of sub-task 4.4 nor the AHCI driver of 4.7 will
ever find anything upon it, and no setting in its firmware would give them
something to find. The kernel told its owner the machine had no disk, of a laptop
that had just booted from its own storage; see [`DISK.md`](DISK.md), Section 2.3.

## 2. Two devices, not one

The thing upon the bus is the **host controller**. The thing that holds the data
is a **card**, and it is a second device with a command set of its own.

Most of this driver is that second conversation rather than register programming.
A card must be woken, asked what it is, given an address, asked how large it is,
and selected, before a single block may be read — and each of those is a command
sent through the controller and answered by the card.

This is the first driver in the kernel with that shape. An ATA disk answers
registers; a card answers a protocol carried over registers.

## 3. The controller

| Register | Offset | Use |
| -------- | ------ | --- |
| Block size | `004h` | Fixed at 512 |
| Block count | `006h` | One: each block is a command of its own |
| Argument | `008h` | The card's, not the controller's |
| Transfer mode | `00Ch` | Direction, and that the block count is meaningful |
| Command | `00Eh` | **Written last**: writing it is what issues the command |
| Response | `010h`–`01Ch` | Four registers; a long response fills all four |
| Buffer data port | `020h` | Where a block is moved, a word at a time |
| Present state | `024h` | The inhibits, the buffer flags, and whether a card is there |
| Power control | `029h` | The bus voltage, and the power |
| Clock control | `02Ch` | The divider, and the two enables |
| Timeout control | `02Eh` | Left at its greatest value |
| Software reset | `02Fh` | All, or the command and data lines alone |
| Normal interrupt status | `030h` | Polled; nothing is enabled to signal |
| Error interrupt status | `032h` | What a command failed with |
| Capabilities | `040h` | The base clock |
| Version | `0FEh` | Reported, not acted upon |

Three details are worth stating because each is silent when wrong.

**The command register is written last.** Every other register it reads must be
in place first, because writing it is what starts the command.

**The status bits are enabled although nothing may signal.** No handler is
registered for this controller, so the *signal* enables are cleared — a request
nothing claims is a request nothing claims. But the *status* enables are what
this driver polls, and a controller whose status enable is clear reports nothing
at all: the command would appear to hang forever, which reads as broken hardware.

**The clock divider register holds half of the divider.** The eight-bit field of
the second version of the specification is the divisor divided by two. A divisor
written whole runs the card at twice the rate intended, which is a bus that works
until it does not.

## 4. Bringing up a card

```
CMD0   GO_IDLE_STATE      every card begins here, whatever the firmware left
CMD8   SEND_IF_COND       the version test — and not merely informative
ACMD41 SD_SEND_OP_COND    repeated until the card has finished powering up
  or
CMD1   SEND_OP_COND       the same, for an embedded card
CMD2   ALL_SEND_CID       read and discarded; the command is what moves the card on
CMD3   SEND/SET_RELATIVE_ADDRESS
CMD9   SEND_CSD           the capacity
CMD7   SELECT_CARD
CMD16  SET_BLOCKLEN       512
```

**CMD8 is not merely informative.** A card of the second version will not
complete its power-up sequence unless it has been asked, so the answer decides
both the kind of card and the argument of the command that follows. A card of the
first version does not answer at all, which is not an error and must not be
treated as one.

**Which power-up command a card answers is how its kind is established.** An SD
card answers ACMD41 — CMD55 followed by CMD41 — and an embedded MultiMediaCard
does not implement ACMD41 at all; it answers CMD1. The driver tries the first and
falls back to the second, which is the whole reason it exists: the machine that
reported the fault has no removable card, and a driver that gave up when ACMD41
went unanswered would find nothing there.

**An embedded card is given an address rather than reporting one.** CMD3 asks an
SD card what address it has chosen and *tells* an embedded card what address to
use. Any value but zero will serve; zero is what deselects.

## 5. How large the card is

This is the arithmetic in the driver most likely to be wrong and least likely to
say so.

There are two encodings of a card's size in the card specific data, chosen by the
`CSD_STRUCTURE` field, and they differ in every respect that matters:

| | Version 1 | Version 2 |
| --- | --- | --- |
| Fields read | `C_SIZE`, `C_SIZE_MULT`, `READ_BL_LEN` | `C_SIZE` alone |
| Where `C_SIZE` sits | bits 73:62 | bits 69:48 |
| Its width | 12 bits | 22 bits |
| The units | bytes, after two multiplications | 512 kibibytes |

A capacity computed by the wrong one of them is not a small error. It is wrong by
a factor of thousands — and a block layer told a card is larger than it is will
read beyond the end of it and be answered with nothing.

There is a second trap on top of the first. **The response registers hold the
card specific data with its low eight bits removed**, the CRC and the end bit
having been stripped by the controller, so bit *N* of the specific data lies at
bit *N* − 8 of the response. Every field in the implementation is therefore named
by its position in the specific data and shifted by that eight in one place,
because reading the specification against code that has already subtracted is how
a field ends up one nibble from where it belongs.

A structure this driver does not know yields **zero** rather than a guess, and a
card whose capacity is zero is not registered.

## 6. A transfer

One block, one command — CMD17 to read and CMD24 to write — repeated for a
request of several. The controller can master the bus and this driver does not
ask it to: direct memory access here means composing a descriptor table in a
second format, for a second engine, with a second set of alignment rules, and
the AHCI driver of sub-task 4.7 already carries the cost of that arrangement
where it buys the most. Here it would buy a faster path to a medium that is
itself the slow part.

Every block therefore moves through the buffer data port a word at a time, as the
ATA driver of sub-task 4.4 moves a sector. The caller's buffer needs no alignment
and no residence beyond the call, which is the compensation.

**The argument is a block number or a byte offset, according to the card.** Bit
30 of the operating conditions register — the card capacity status — is the
authority. A byte-addressed card given a block number reads from 512 times the
wrong place, and upon block zero the two are the same, which is what makes the
mistake survive a casual test.

**A transfer is not finished when the last word has moved.** A write is still in
the card, and the transfer complete bit is what says it has been committed. A
caller told a write succeeded before that has been told something the driver does
not know.

## 7. Verification

### 7.1 The arithmetic, upon values composed rather than obtained

`KernelVerifySdhci` asserts the two decisions that need no hardware.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| A version 2 capacity is `(C_SIZE + 1) * 1024` blocks | Section 5: a capacity wrong by a factor of thousands |
| The greatest version 2 capacity does not overflow or truncate | A 32-bit intermediate in a computation whose answer needs more |
| A version 1 capacity applies both the multiplier and the block length | The same, in the other direction |
| The two encodings **disagree** upon the same bits | This is what makes `CSD_STRUCTURE` load-bearing. If they agreed, a driver that ignored the field would pass every row above by accident |
| An unknown structure yields zero | A capacity guessed at, and a card registered as larger than it is |
| CMD17 is composed with its index, the data bit, both checks and a 48-bit response | — |
| A **long** response is composed with the index check **off** | A response of 136 bits carries no command index. Left checked, every CMD2 and CMD9 reports an index error and the card is never identified — which presents as a machine with no storage |
| An **unchecked** response is composed with neither check | The operating conditions register comes back with neither a CRC nor an index, both fields carrying part of the register instead. Checking either rejects a card that answered correctly |

The values are composed because no card available to this project uses the first
encoding — it belongs to cards of two gibibytes and below — and because a command
register that is wrong cannot be observed to be wrong except by the card failing
to appear. The helper that composes them takes each field's position **in the
card specific data**, so that a reader may check it against the specification
without doing the subtraction of Section 5 in their head.

### 7.2 The transfers, where a card answered

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| A block read twice into **differently seeded** buffers agrees over the whole block | A transfer shorter than a block. The two buffers still differ wherever the card did not write |
| Nothing beyond the block was written | A transfer longer than one |
| A two-block read begins with what a one-block read returned, and its second block is what a read of the following block returns | Two commands whose arguments did not advance, or advanced by the wrong unit |
| A range beyond the card, a count of zero, a count beyond the driver's limit and an absent buffer are each refused | A request the controller was asked to attempt that it should never have seen |
| With the option given: a pattern written to the final block reads back byte for byte, and the block is restored | The write path entirely, and any failure of the completion wait |

The seeding of the first row is not incidental. It was added to the AHCI driver's
self-test after two reads compared against each other passed with a region
descriptor's byte count halved; the same reasoning applies here and the same
assertion is made.

### 7.3 What is still not asserted

**The byte-addressed and block-addressed forms of the transfer argument are not
told apart by any assertion.** Block zero is byte zero, so the two agree there;
and a card read consistently by the wrong form returns data that is
self-consistent, so the two-block assertion of Section 7.2 passes. The mistake is
caught only from outside — a volume that will not mount — and that is how it is
tested here, by mounting one.

**The embedded MultiMediaCard path is written from the specification and has not
been run.** No emulator available to this project presents an eMMC part: QEMU's
`sd-card` is an SD card, and VirtualBox has no SD host controller at all. What can
be asserted without one is asserted — the command composition, the capacity
arithmetic — and CMD1 itself awaits the machine that reported the fault. This is
recorded rather than papered over.

### 7.4 The machines used

QEMU, with an SD card attached to an `sdhci-pci` controller:

```sh
qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso \
    -device sdhci-pci,id=sd \
    -drive if=none,id=sdcard,file=sd.img,format=raw \
    -device sd-card,drive=sdcard,bus=sd-bus \
    -display none -serial file:sd.log
```

Both capacity encodings are reached by changing the size of the image alone: an
image of 16 mebibytes presents as a byte-addressed SD card and one of four
gibibytes as a block-addressed SDHC card, and the driver's two paths are
exercised without either being asked for.

**VirtualBox presents no SD host controller**, so this driver's live half has no
second machine. The half that needs no hardware runs upon both and passes upon
both. That is stated here rather than left for a reader to notice.

## 8. Limitations

1. **One card, one slot.** A controller with several slots is programmed as
   though it had the first alone. Nothing available here has more.
2. **Programmed input/output.** Every word crosses the buffer data port. The
   controller can master the bus; see Section 6 for why it is not asked to.
3. **One block per command.** A multi-block transfer is a command per block
   rather than CMD18 with a stop. It is simpler and it is slower, and the block
   layer above bounds how much either matters.
4. **The eMMC path is untested.** Section 7.3.
5. **No high-speed modes.** The bus runs at 25 MHz once the card is selected.
   The four-bit data width, the high-speed timings and the UHS modes each need a
   negotiation of their own and each is a way to make an untested path faster.
6. **No hot plug.** The slot is examined once, at initialisation. A card
   inserted afterwards is not noticed, there being nothing yet to tell.
7. **No card removal detection during a transfer.** A card pulled mid-command is
   reported as a timeout, which is what it looks like from here.
8. **No error recovery beyond a reset of the command and data lines.** What to
   retry and how often is a policy, and the block layer is where a policy
   belongs.
