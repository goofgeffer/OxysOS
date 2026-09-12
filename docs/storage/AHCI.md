<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The AHCI Disk

**Phase**: 4, sub-task 4.7, of [`PLAN.md`](../project/PLAN.md).

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6. Every assertion of
hardware behaviour below carries a citation, and every specification named is
registered in [`REFERENCES.md`](../project/REFERENCES.md).

**Implementation**: [`../../drivers/ahci/ahci.c`](../../drivers/ahci/ahci.c),
[`../../kernel/include/oxys/dev/storage/ahci.h`](../../kernel/include/oxys/dev/storage/ahci.h).

## 1. Why this driver exists

Sub-task 4.4 drives the ATA command block registers through I/O ports, which is
an IDE controller and nothing else. This was reported from a real machine as a
kernel that finds no disk: a firmware presenting its serial ATA controller in
**AHCI** mode puts the disks behind memory-mapped registers that answer at no I/O
port whatever, and the symptom of that is exactly the symptom of having no disk
at all.

Sub-task 4.4 was then taught to say which it was — see
[`DISK.md`](DISK.md), Section 2.2 — and saying so is not driving it. This is the
driving.

It is worth stating plainly what did **not** happen here. The ATA driver was not
extended. The two speak the same command set and reach it by entirely different
means, and the difference is not a register layout:

| | ATA, sub-task 4.4 | AHCI, this sub-task |
| --- | --- | --- |
| The command | Written a byte at a time into I/O ports | Composed in memory as a FIS |
| Who moves the data | The processor, one word per `IN` | The adaptor, by bus mastering |
| Where the data goes | Wherever the processor puts it | The caller's own pages, named to the adaptor by **physical** address |
| What a transfer costs | The processor entirely | One command issue and one poll |

The third row is the substance. Every structure this driver composes is read by
the device itself, so it is described by physical address, and every buffer a
caller supplies must be resident while the transfer runs.

## 2. Finding the adaptor, and taking it

An AHCI controller is class `0x01`, subclass `0x06`, programming interface
`0x01`. The subclass alone is not enough: a serial ATA controller may present
other interfaces, and the driver would then read registers that are not there.

Three things must happen before a register may be believed.

**Memory space and bus mastering are enabled.** The first because the mapping
would otherwise decode to nothing; the second because the adaptor would accept a
command and never fetch it. Both are bits of the PCI command register, and
neither can be assumed: firmware that booted from a disk in AHCI mode will have
set them, and firmware that did not, did not.

**The registers are mapped uncached.** They are described by the sixth base
address register — `ABAR` — which describes memory and not ports, and the mapping
is made with `PAGE_ENTRY_CACHE_DISABLE`. A cached mapping of a device register
returns whatever the processor last read, which for a status register polled in a
loop is the first value it ever held.

**The firmware is asked to let go.** Where `CAP2.BOH` says the adaptor implements
the BIOS/OS handoff and `BOHC.BOS` says the firmware still owns it, `BOHC.OOS` is
set and the driver waits for the firmware to clear its own bit. This is not
ceremony: a system management interrupt may be servicing the controller on the
firmware's behalf — presenting a disk as a floppy drive to an operating system
that does not know AHCI — and reprogramming the ports underneath that leaves the
firmware writing to registers this driver has taken.

**`GHC.AE` is set before anything else is read.** An adaptor that has not been
enabled presents its registers in a legacy arrangement, so the ports implemented
register read from that arrangement is not the ports implemented register.

## 3. The registers, and where they are

| Register | Offset | What this driver uses it for |
| -------- | ------ | ---------------------------- |
| `CAP` | `00h` | The command slot count, and whether the adaptor can address more than four gibibytes |
| `GHC` | `04h` | `AE`, bit 31, which enables AHCI |
| `PI` | `0Ch` | Which ports exist. **Not** how many; the implemented ports need not be consecutive |
| `VS` | `10h` | Reported, not acted upon |
| `CAP2` | `24h` | `BOH`, bit 0: whether the handoff exists |
| `BOHC` | `28h` | The handoff itself |

The block for port *x* begins at `100h + x * 80h`, and within it:

| Register | Offset | Use |
| -------- | ------ | --- |
| `PxCLB`, `PxCLBU` | `00h`, `04h` | The physical address of the command list |
| `PxFB`, `PxFBU` | `08h`, `0Ch` | The physical address of the received FIS area |
| `PxIS`, `PxIE` | `10h`, `14h` | The interrupt status, which is polled; the enable, which is left at zero |
| `PxCMD` | `18h` | `ST` bit 0, `FRE` bit 4, `FR` bit 14, `CR` bit 15 |
| `PxTFD` | `20h` | The device's status byte in its low eight bits |
| `PxSIG` | `24h` | What is attached |
| `PxSSTS` | `28h` | Whether the link is up |
| `PxSERR` | `30h` | Cleared by writing back what it holds |
| `PxCI` | `38h` | The command issue register: a bit per slot |

`PI` being a bitmap rather than a count is the first thing a careless driver gets
wrong. The board this was developed against implements ports 0, 1, 2, 3, 4 and 5;
a board that implements 0 and 2 alone is entirely ordinary, and a driver that
walked *n* consecutive ports would program a port that does not exist and skip one
that does.

## 4. Whether a port has anything upon it

Two fields of `PxSSTS`, read **together**:

- `DET`, bits 3:0, is 3 when a device is present and communication is
  established. A value of 1 is presence detected without communication — a device
  that has not finished negotiating — and a driver that accepted it would issue a
  command and wait out its whole patience for a reply that cannot come.
- `IPM`, bits 11:8, is 1 when the interface is active. A value of 2 is a partial
  power state and 6 a slumbering one; the device is present in both, so a driver
  reading the detection alone cannot tell them from a port ready to answer.

Between the two lies the negotiated speed, in bits 7:4, which is **not** part of
either. A mask that swept it in would reject every port upon every machine that
negotiated anything but the slowest link.

`PxSIG` then says what is attached: `00000101h` a disk, `EB140101h` a packet
device, `C33C0101h` an enclosure services device, `96690101h` a port multiplier.
Every one of them ends in `0101h`, so a comparison of the low half alone calls a
packet device a disk — and this driver would then issue `READ DMA EXT` to
something that answers only the packet interface.

## 5. The memory a port is given

One page per active port, laid out so that every structure lands upon the
boundary the specification requires:

| Offset | Structure | Alignment required |
| ------ | --------- | ------------------ |
| `0000h` | Command list, 32 headers of 32 bytes | 1024 |
| `0400h` | Received FIS area, 256 bytes | 256 |
| `0500h` | Command table of slot 0 | 128 |
| `0580h` | Its region descriptors | — |
| `0800h` | Where `IDENTIFY DEVICE` is read into | — |

A frame is page-aligned, so an offset that is a multiple of its own requirement
is aligned in the absolute sense too. One allocation per port is what allows the
whole of a port's memory to have one physical address, which is the address the
adaptor is given.

The frame is taken **below four gibibytes** where `CAP.S64A` says the adaptor
cannot address more. An adaptor told to fetch its command list from an address it
can express only in 32 bits fetches from the low half of it, which is some other
page of this kernel — read as a command list, and executed.

### 5.1 Cache coherence, and why nothing is flushed

These structures are ordinary write-back memory that the adaptor reads by bus
mastering. Intel SDM, Volume 3A, Section 11.3.4 provides that the processor's
caches are kept coherent with such accesses by the hardware, so there is nothing
to flush and no uncached mapping to make.

What must be got right is the **order** of the writes. Every structure is
addressed through a `volatile` pointer and the write to `PxCI` that issues the
command is volatile too, so the compiler may not move the command ahead of the
description of it. That is the whole of the ordering this driver needs on this
architecture, and it is obtained without a single barrier instruction.

## 6. A command

Slot zero, always. One command is issued, waited for, and followed by the next.

An adaptor accepts thirty-two at once and completes them in whatever order it
likes, and this driver uses one of them. That is the same discipline the ATA
driver keeps and it is kept for the same reason: the interrupt flag is clear
throughout initialisation, so there is nothing to be woken by, and a driver that
queued commands it could not be told about would poll thirty-two slots instead of
one and be no faster for it. Queuing belongs with the scheduler of Phase 7,
which can wait upon a command without occupying the processor.

### 6.1 The command header

Thirty-two bytes, of which this driver composes the first sixteen. The first
double word carries three things at once and is the one field in the driver
composed from three numbers:

| Bits | Field | What a mistake does |
| ---- | ----- | ------------------- |
| 4:0 | The command FIS length, **in double words** | A length in bytes is 20, which does not fit five bits and becomes 4 |
| 6 | Write | A command whose direction is wrong transfers the disk **into** the buffer the caller meant to write from, and reports success |
| 31:16 | The number of region descriptors | Zero here transfers nothing and succeeds |

It is exposed as `AhciDescribeCommand` for exactly that reason: three fields, one
word, and every mistake silent.

### 6.2 The region descriptors

A buffer is contiguous to the processor and need not be to the device: the pages
behind it are wherever the allocator had them. Each page therefore becomes a
descriptor of its own, translated through the page tables at the moment the
command is composed.

The byte count in a descriptor is **one less** than the bytes it covers. That is
the specification's encoding and it is the likeliest single mistake in the file —
see Section 8.2 for what happens when it is got wrong, which is less than one
would hope.

A descriptor's address must be even, the specification reserving its low bit. A
request whose buffer is at an odd address is therefore **refused** rather than
rounded: rounding it would transfer to the byte below, which is a silent
corruption of whatever lies there.

### 6.3 The FIS

A Register Host to Device FIS, type `27h`, twenty bytes:

| Byte | Content |
| ---- | ------- |
| 0 | `27h` |
| 1 | Bit 7 set: this is a command and not a register update |
| 2 | The command |
| 4, 5, 6 | The low three bytes of the address |
| 7 | The device register, with bit 6 set for logical block addressing |
| 8, 9, 10 | The high three bytes of the address |
| 12, 13 | The sector count |

Every command this driver issues is an extended one — `READ DMA EXT`,
`WRITE DMA EXT` — so there is no 28-bit path to get wrong. What can be got wrong
is the address, which is composed from six bytes across two halves of the FIS: a
byte written into the wrong one of them addresses a sector some multiple of
sixteen megabytes away, which is a read that succeeds and returns the wrong data.

### 6.4 Completion

`PxCI` bit 0 clears when the adaptor has finished. `PxIS.TFES`, bit 30, is set
where the device reported an error, and `PxTFD` bit 0 carries the error onward.
Both are read: the first distinguishes a failed command from a slow one, and the
second is what a device that failed without raising the first still says.

A command that never completes exhausts the driver's patience —
`AHCI_WAIT_LIMIT` register reads — and is counted as a **timeout**, apart from an
error. The two mean opposite things and a figure that added them would hide both.

## 7. What a write is

`WRITE DMA EXT`, followed by `FLUSH CACHE EXT` within the same sequence.

The flush is part of the write and not a courtesy after it. A device that has
accepted the data into its own cache reports success and may lose it, and a
caller told the write succeeded has no way to discover that.

## 8. Verification

`KernelVerifyAhci` asserts three decisions that need no hardware and then the
transfers themselves where a disk answered.

### 8.1 The decisions, upon values no board here produces

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| A port with `DET` 3 and `IPM` 1 is usable | — |
| The negotiated speed, between the two fields, is not read as part of either | Every port upon every machine that negotiated better than the slowest link rejected |
| `DET` of 0, 1 or 4 is not usable | A command issued to a port whose link is not up, and the driver's whole patience spent waiting for it |
| `IPM` of 0, 2 or 6 is not usable | The same, for a port whose interface is asleep — indistinguishable from a ready port if the detection is read alone |
| The four signatures are told apart | A packet device driven as a disk: `READ DMA EXT` issued to something that answers only the packet interface |
| An unrecognised signature is not taken for a device | Every signature ends in `0101h`, so a comparison of the low half calls all four a disk |
| The command header places the FIS length, the write bit and the region count where they belong | Section 6.1: a FIS four times too long, a transfer in the wrong direction, or a transfer of nothing |

No board available to this project presents a packet device, an enclosure or a
port multiplier upon an AHCI port, and none presents a port whose interface has
gone to sleep. Each of those is a value the driver must read correctly and none
can be produced here, so each decision is asked directly of the value the
hardware would have given — the same method the ATA driver's channel addressing
is asserted by.

### 8.2 The transfers, and the assertion that was not strong enough

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| A sector read twice into **differently seeded** buffers agrees over the whole sector | A transfer shorter than a sector. The two buffers still differ wherever the device did not write |
| Nothing beyond the sector was written | A region byte count one too large, writing past what the caller asked for |
| A two-sector read begins with what a one-sector read returned | The count register mishandled, so the second sector overwrites the first |
| Its second sector is what a read of the following address returns | Loss of synchronisation between sectors |
| A sector beyond the 28-bit limit reads | The address composed across the two halves of the FIS wrongly |
| A range beyond the capacity, a count of zero, a count beyond the driver's own limit, an absent buffer and an **odd** buffer are each refused | Section 6.2, and a transfer the adaptor was asked to attempt that it should never have seen |
| With the option given: a pattern written to the final sector reads back byte for byte, and the sector is restored to what it held | The write path entirely, and any failure of the flush |

**The seeding is the point of the first row, and it was added because the test
failed to catch a real fault.** The first form compared two reads into buffers
that both already held the previous read, and it passed with the region byte
count halved and again with it one too large. Two reads compared against each
other establish less than they appear to: a transfer that is consistently the
wrong length leaves both buffers holding the same wrong thing. Seeding them with
different bytes is what gives the comparison its force — wherever the device did
not write, the two still differ.

### 8.3 What is still not asserted

A region byte count **one too large** — the descriptor holding the length rather
than the length less one — is not caught, and cannot be by any assertion
available here. The descriptor is a *capacity* and the command's sector count is
the *length*; an adaptor transfers what the command asked for and never reaches
the extra byte. It is wrong by the specification, and it also leaves the byte
count even where the encoding requires it odd, so an adaptor that validated the
field would reject it. This one does not, and neither does the second one tested.

This is recorded rather than papered over. The failing case is a machine this
project does not have.

### 8.4 The machines used

`make verify` runs upon the q35 board, whose AHCI controller answers and carries
the boot ISO as a packet device upon port 2 — so the adaptor, the handoff, the
port preparation and the signature are all exercised at every boot, and the disk
half reports that it had nothing to transfer.

The transfers are exercised upon the same board with a disk attached:

```sh
qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
    -cdrom build/oxys.iso \
    -drive file=disk.img,format=raw,if=none,id=sata0 \
    -device ide-hd,drive=sata0,bus=ide.0 \
    -display none -serial file:ahci.log
```

and upon VirtualBox, whose Intel ICH8-M AHCI controller is a second silicon
design and not a second copy of the first. Both are recorded in
[`TESTING.md`](../project/TESTING.md).

## 9. Limitations

1. **One command at a time.** Slot zero, polled. Native command queuing is what
   makes an AHCI disk fast and it needs a scheduler to wait upon; see Section 6.
2. **Polled, not interrupt-driven.** `PxIE` is left at zero and no handler is
   registered, so no port may raise a request that nothing claims. A transfer
   occupies the processor entirely.
3. **128 sectors to a command.** Sixty-four kibibytes, which is seventeen region
   descriptors at worst. The device's own limit is 65536 sectors; reaching it
   needs a larger command table, which needs more than one page per port.
4. **No packet commands.** A packet device upon a port is recognised, reported
   and left alone, exactly as the ATA driver leaves one.
5. **No port multiplier.** A port whose signature says a multiplier is named and
   not followed. Following one means addressing each device behind it separately,
   which is a second layer of enumeration.
6. **No hot plug.** The ports are enumerated once, at initialisation. A disk
   attached afterwards is not noticed, there being nothing yet that could be told
   about it.
7. **No error recovery.** A command that fails is reported, not retried, and a
   port left in an error state is not restarted. What to retry and how often is a
   policy, and the block layer is where a policy belongs.
8. **A buffer above four gibibytes is refused upon a 32-bit adaptor** rather than
   bounced through a low buffer. Refusing is honest and copying is not
   free; nothing in this kernel yet allocates that high.
