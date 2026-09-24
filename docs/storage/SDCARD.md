<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The SD Card and the Embedded MultiMediaCard

**Phase**: sub-task 4.8 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../drivers/sdhci/sdhci.c`](../../drivers/sdhci/sdhci.c),
[`../../kernel/include/oxys/dev/storage/sdhci.h`](../../kernel/include/oxys/dev/storage/sdhci.h).
**Specifications**: SD Host Controller Simplified Specification 4.20; SD
Physical Layer Simplified Specification 8.00; JEDEC JESD84-B51 (eMMC). All in
[`../project/REFERENCES.md`](../project/REFERENCES.md).

The driver for storage behind an SD host controller: a removable SD card, or an
embedded MultiMediaCard soldered to the board. Many inexpensive laptops have no
other storage, and their controller is classed as a system peripheral (class
`0x08`, subclass `0x05`), not mass storage, so neither [`DISK.md`](DISK.md) nor
[`AHCI.md`](AHCI.md) finds anything on them. The machine of
[`../project/TESTING.md`](../project/TESTING.md) that reported the problem, an
HP Laptop 14-dq0052dx, is one.

## 1. Two devices

The **host controller** is on the PCI bus; the **card** holds the data and has a
command set of its own. Before a block can be read, a card must be woken, asked
what it is, given an address, asked its size and selected, each by a command sent
through the controller and answered by the card. Most of this driver is that
protocol, not register programming.

## 2. The controller

| Register | Offset | Use |
| -------- | ------ | --- |
| Block size | `004h` | 512 |
| Block count | `006h` | 1; each block is its own command |
| Argument | `008h` | The card's argument |
| Transfer mode | `00Ch` | Direction; block count enable |
| Command | `00Eh` | Written last: writing it issues the command |
| Response | `010h`–`01Ch` | Four registers; a long response fills all four |
| Buffer data port | `020h` | A block moves through it a word at a time |
| Present state | `024h` | Inhibits, buffer flags, card present |
| Power control | `029h` | Bus voltage and power |
| Clock control | `02Ch` | Divider and the two enables |
| Timeout control | `02Eh` | Left at its maximum |
| Software reset | `02Fh` | All, or the command and data lines |
| Normal interrupt status | `030h` | Polled |
| Error interrupt status | `032h` | Why a command failed |
| Capabilities | `040h` | The base clock |
| Version | `0FEh` | Reported only |

- **The command register is written last**, because writing it starts the
  command with whatever the other registers hold.
- **Status enables are set; signal enables are clear.** No handler is registered,
  so nothing may interrupt; but a clear status enable reports nothing at all, and
  every command would appear to hang.
- **The clock divider field holds half the divisor** (version 2 of the
  specification). Writing the divisor whole runs the card at twice the intended
  rate: a bus that works until it does not.

## 3. Bringing up a card

```
CMD0   GO_IDLE_STATE            every card starts here, whatever the firmware left
CMD8   SEND_IF_COND             the version test
ACMD41 SD_SEND_OP_COND          repeated until power-up completes (SD)
  or CMD1 SEND_OP_COND          the same, for eMMC
CMD2   ALL_SEND_CID             read and discarded; it moves the card on
CMD3   SEND/SET_RELATIVE_ADDR
CMD9   SEND_CSD                 the capacity
CMD7   SELECT_CARD
CMD16  SET_BLOCKLEN             512
```

- **CMD8 decides the next argument.** A version 2 card will not finish power-up
  unless asked; a version 1 card does not answer, which is not an error.
- **The power-up command a card answers tells its kind.** An SD card answers
  ACMD41 (CMD55 then CMD41); eMMC does not implement it and answers CMD1. The
  driver tries the first and falls back to the second: a driver that stopped at
  an unanswered ACMD41 finds nothing on an eMMC laptop.
- **eMMC is given an address.** CMD3 asks an SD card for the address it chose,
  and tells an eMMC part which to use. Any value but zero, which deselects.

## 4. Capacity

The card specific data (CSD) encodes size in one of two ways, selected by
`CSD_STRUCTURE`:

| | Version 1 | Version 2 |
| --- | --- | --- |
| Fields | `C_SIZE`, `C_SIZE_MULT`, `READ_BL_LEN` | `C_SIZE` |
| `C_SIZE` bits | 73:62 (12 bits) | 69:48 (22 bits) |
| Unit | bytes, after two multiplications | 512 KiB |

The wrong encoding is wrong by a factor of thousands, and a block layer told a
card is larger than it is reads past its end.

**The response registers hold the CSD without its low eight bits** (the
controller strips the CRC and end bit), so CSD bit *N* is response bit *N* − 8.
Fields are named by their CSD position and shifted by eight in one place, so the
code can be checked against the specification without doing the subtraction.

An unknown structure yields a capacity of zero, and a card of zero capacity is
not registered.

## 5. Transfers

- **One block per command**: CMD17 reads, CMD24 writes.
- **Programmed I/O.** Every word crosses the buffer data port. Bus mastering
  would need a second descriptor format and engine for a medium that is itself
  the slow part. The caller's buffer needs no alignment and no lifetime beyond
  the call.
- **The argument is a block number or a byte offset**, by bit 30 of the
  operating conditions register (card capacity status). A byte-addressed card
  given a block number reads 512 times too far; at block zero the two agree,
  which is how the mistake survives a casual test.
- **A write is complete at transfer complete**, not when the last word moves:
  until then it is still in the card, and reporting success earlier reports what
  the driver does not know.

## Verification

`KernelVerifySdhci` in [`../../kernel/test/storage/stack.c`](../../kernel/test/storage/stack.c).
The arithmetic is asserted on composed values, since no available card uses the
version 1 encoding (cards of 2 GiB and below) and a wrong command register shows
only as a card that never appears. The composing helper takes each field's CSD
position, so it can be checked against the specification directly.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| A version 2 capacity is `(C_SIZE + 1) * 1024` blocks. | A capacity wrong by thousands. |
| The largest version 2 capacity neither overflows nor truncates. | A 32-bit intermediate. |
| A version 1 capacity applies both multiplier and block length. | The same, the other way. |
| The two encodings disagree on the same bits. | Without this, a driver ignoring `CSD_STRUCTURE` passes the rows above by accident. |
| An unknown structure yields zero. | A guessed capacity. |
| CMD17 is composed with its index, the data bit, both checks and a 48-bit response. | A malformed read command. |
| A long response is composed with the index check off. | Every CMD2 and CMD9 failing an index check that a 136-bit response cannot pass: a machine with no storage. |
| An unchecked response (the OCR) has neither check. | A correct answer rejected; its CRC and index fields carry register bits. |

Where a card answers, the transfers are asserted too:

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| A block read twice into differently seeded buffers agrees over the whole block. | A short transfer; the seeds still differ where nothing was written. |
| Nothing beyond the block was written. | A long transfer. |
| A two-block read starts with the one-block read and continues with the next block. | An argument that does not advance, or advances by the wrong unit. |
| Out-of-range, zero-count, over-limit and bufferless requests are refused. | The controller asked to attempt what it should never see. |
| With the write option, a pattern written to the last block reads back, and the block is restored. | The write path and the completion wait. |

Not asserted: the two argument forms are not told apart (a card read consistently
in the wrong form is self-consistent). Mounting a volume from the card is the
test that catches it.

To attach a card under QEMU (a 16 MiB image presents a byte-addressed SD card, a
4 GiB image a block-addressed SDHC card, so both paths are reached by size
alone):

```sh
qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso \
    -device sdhci-pci,id=sd \
    -drive if=none,id=sdcard,file=sd.img,format=raw \
    -device sd-card,drive=sdcard,bus=sd-bus \
    -display none -serial file:sd.log
```

VirtualBox has no SD host controller, so only the hardware-free half runs there.

## Limitations

1. The eMMC path (CMD1, the assigned address) is written from the specification
   and has not run: no available emulator presents an eMMC part.
2. One card, one slot; a multi-slot controller is driven as its first slot.
3. Programmed I/O only.
4. One command per block; no CMD18 with a stop.
5. The bus runs at 25 MHz, one data line; no 4-bit width, high-speed or UHS
   modes.
6. The slot is examined once; no hot plug, and a card removed mid-command is
   reported as a timeout.
7. Error recovery is a reset of the command and data lines; retry policy belongs
   to the block layer.
